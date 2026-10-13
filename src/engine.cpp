// Hooks inside the Teardown engine, found by reverse engineering (see
// docs/REVERSE_ENGINEERING.md). Applied on the first Present, once the Steam
// DRM has decrypted the code. Everything is located at run time with byte
// patterns, so a game update that moves the code simply disables synchronised
// stereo (the mod then falls back to plain alternate-eye rendering) instead of
// crashing.
//
// Synchronised stereo
// -------------------
// Teardown's renderer cannot draw two views in one frame (its per-frame GPU
// data would be overwritten - tested: the GPU hangs). Instead every stereo
// pair uses two engine frames that show the SAME simulation state:
//   frame A: normal frame. The Lua mod places the camera on the left eye.
//   frame B: a "zero time" frame. Game::update still runs (scripts, tools,
//            animations - so everything stays consistent) but its time is
//            taken back out of the fixed-step accumulators, so the physics
//            loop that follows runs no step, and frame time is ~0. The right
//            eye is rendered from the left eye's camera moved by the eye offset.
// Frame A advances the game by the headset's display interval (or, without a
// headset, by its own time plus frame B's), so the simulation runs at real speed.
#include "engine.h"
#include "config.h"
#include "frameb.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "particles_vr.h"
#include "perf.h"
#include "prof.h"
#include "sampler.h"
#include "taa_vr.h"
#include "vrmath.h"

#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <intrin.h>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// (defined further down, outside the anonymous namespaces)
static BYTE* EntityOfHandle(BYTE* scene, int h);   // the engine's script handle -> entity lookup
static bool EntityLookupReady();                    // ... found (InitOffArm)
extern uint32_t g_offGameScene;                     // Game -> scene
extern uint32_t g_gameStep;                         // Game -> physics step counter (InitParticleHook; 0 = not found)

namespace {

BYTE* g_base = nullptr;
BYTE* g_text = nullptr;
size_t g_textSize = 0;

using PFN_Update = void(__fastcall*)(void* game);
PFN_Update Real_Update = nullptr;

// Game object offsets (resolved at run time, Teardown 2.0.1 values in comments).
uint32_t g_offDt = 0;     // frame delta time                   (0x204)
uint32_t g_offAcc1 = 0;   // fixed-step time accumulator         (0x214)
uint32_t g_offAcc2 = 0;   // second fixed-step accumulator       (0x218)
bool g_ready = false;

// First-person tool placement. In first person the engine ignores
// SetToolTransformOverride (it only honours it in third person); we apply it.
using PFN_ToolAnim = __int64(__fastcall*)(void* self, float dt, void* a3, void* a4);
PFN_ToolAnim Real_ToolAnim = nullptr;
uint32_t g_offToolPlayer = 0;   // ToolAnim -> Player*               (0x58)
uint32_t g_offToolOverride = 0; // Player -> override transform      (0xa0c)
uint32_t g_offToolFinal = 0;    // Player -> final tool transform    (0x844)
uint32_t g_offToolFlag = 0;     // Player -> override flag           (0xa38)
uint32_t g_offToolAnimPose = 0; // ToolAnim -> animated pose before the desktop offsets (0x68)
uint32_t g_offToolEntry = 0;    // ToolAnim -> current tool entry    (0x30)
uint32_t g_offToolName = 0;     // tool entry -> inline name         (0x18)
bool g_vrActive = false;

// Tools added by mods ("script tools"). On a desktop screen the engine shows them
// where their script puts them each frame with SetToolTransform (eye space,
// smoothed by the script-tool animator 0x451de0, no prefab locations involved);
// with the override flag set (VR) it skips that and copies the override instead.
// SetToolTransform and SetToolTransformOverride write the SAME player field
// (+0xa0c, only the override sets the flag +0xa38) and the scripts run in an
// unknown order: whichever ran last won (the mod's desktop pose replaced the VR
// hand, or the hand erased the mod's pose and its animations). The Lua binding of
// SetToolTransform is therefore hooked to keep the two apart.
using PFN_LuaFn = __int64(__fastcall*)(void* ctx, void* args, void* a3, void* a4);
PFN_LuaFn Real_SetToolTransform = nullptr;
uint32_t g_offToolHandSet = 0;   // Player -> hand pose set this frame (SetToolHandPoseLocalTransform) (0xa39)
uint32_t g_offToolHandR = 0;     // Player -> right hand pose, tool space                             (0xa70)
uint32_t g_offToolHandRUsed = 0; // Player -> right hand used                                         (0xa8c)
Pose g_scriptToolT;              // the mod's SetToolTransform of this frame (local player)
void* g_toolScriptCtx = nullptr; // the script that called it (WorldPixelView: its tool's markers)
uint64_t g_toolScriptFrame = 0;  // engine frame of that call
bool g_scriptToolTSet = false;
std::string g_scriptToolName;    // mod tool whose desktop pose is being smoothed ("" = none)
Pose g_scriptToolFlat;           // its smoothed desktop pose (eye space)
EngineToolInfo g_toolInfo;       // reported to the Lua mod (EngineGetToolInfo)
std::atomic<bool> g_sledgeHeld{ false };    // the VR hand holds the game's sledge (Hook_ToolAnim; EngineHoldingSledge)
std::atomic<uint64_t> g_sledgeFrame{ 0 };   // ... seen at this engine frame

// Dual wield (see DualEye, further down): mod tools made of two guns that one mouse button fires
// alternately. Total Overdose (workshop 3449723355) draws both pistols in one tool and, on each
// InputPressed("usetool"), calls GetPlayerEyeTransform() twice (cannon1, cannon2) and fires ONE shot
// from the eye of its hidden alternation (`shoot1`): muzzle = eye + rot x (+-side, 0, fwd) + world
// (0, up + crouchUp x crouch, 0), direction = rot's -Z.
struct DualProfile { const char* tool; float side, fwd, up, crouchUp; int calls; };
const DualProfile kDualProfiles[] = {
    { "totaloverdose", 0.25f, -0.5f, -0.05f, 0.05f, 2 },
    { "totaloverdose2", 0.25f, -0.5f, -0.05f, 0.05f, 2 },   // (its silenced variant, SEU cheat)
};
constexpr int kDualProfileCount = sizeof(kDualProfiles) / sizeof(kDualProfiles[0]);
struct DualGun
{
    Vec3 m;                    // muzzle in its body (TDVRG)
    uint64_t mFrame = 0;
    Vec3 target;               // laser target, world (TDVRK: muzzle at reception + the offset sent)
    uint64_t tFrame = 0;
};
struct DualState
{
    const DualProfile* prof = nullptr;   // the held tool's profile (Hook_ToolAnim, kind 2)
    uint64_t profFrame = 0;
    bool off[kDualProfileCount] = {};    // turned off for the session (a shot frame with too few eye calls)
    bool hooked = false;                 // GetPlayerEyeTransform hooked (InitDualWield)
    DualGun gun[2];                      // 0 = off hand (copy), 1 = dominant hand (tool)
    int copy = 0;                        // the off hand's copy (script handle, TDVRV)
    // The click in flight: posted by input.cpp (present thread: g_dualClickGun / g_dualClickId), its
    // down delivered to the game (window procedure, main thread: downId / downFrame).
    uint32_t downId = 0;
    uint64_t downFrame = 0;
    uint64_t downNormal = 0;             // g_normalUpdates then: its shot is in the next normal update
    // Its shot (the first eye call of the tool's script in that update, main thread).
    uint32_t usedId = 0;
    uint64_t shotFrame = 0;
    int shotGun = -1, calls = 0, shots = 0;
    int shortShots = 0;                  // overridden shots in a row with fewer eye calls than the profile
    bool shotOk = false;
    Vec3 shotM;
    Quat shotR;
    float shotDist = 0, shotCrouch = 0;  // (diagnostics)
    bool shotDataNow = false;            // its target came this frame (the Lua mod ran before the tool's script)
    // The mod's tool pose when it does not kick (an off-hand shot must not kick the dominant gun).
    Pose restT;
    bool restOk = false;
    unsigned kicksHeld = 0, overrides = 0, extraCalls = 0;   // (diagnostics)
};
DualState g_dual;
std::atomic<int> g_dualClickGun{ -1 };     // the gun of the click in flight (-1 = none)
std::atomic<uint32_t> g_dualClickId{ 0 };  // its number (each click a new one)
// For input.cpp (present thread: no game memory read there): + 1 dual wield ready (allowed, the
// dominant gun's data fresh), + 2 the off hand's gun too; the game update it was computed in (main
// thread, at each Lua request: DualPublish).
std::atomic<int> g_dualReadyBits{ 0 };
std::atomic<unsigned long long> g_dualReadyAt{ 0 };

// Final render camera. Game::lateUpdate builds the render camera from the
// player's camera transform:  Camera::setTransform(&cam, &player->camPos, &player->camRot).
// In the zero-time right-eye frame the game still runs per-frame camera effects
// (height smoothing after jumps and landings, landing shake...), so that
// transform drifts from the left eye's (measured: up to 43 cm / 18 deg while
// falling) - the two eyes no longer match and the stereo hurts. The right eye
// is therefore rendered from the left eye's actual camera moved by the eye offset.
using PFN_CamSet = void(__fastcall*)(void* cam, const float* pos, const float* rot);
PFN_CamSet Real_CamSet = nullptr;
uint32_t g_offGamePlayer = 0;   // Game -> Player*                       (0x60)
uint32_t g_offCamPos = 0;       // Player -> camera position (vec3)      (0x1d0)
uint32_t g_offCamRot = 0;       // Player -> camera rotation (quat xyzw) (0x1dc)
BYTE* g_gameObj = nullptr;
uint32_t g_offPlayerYaw = 0;    // player entity -> look yaw, radians   (0x704)
uint32_t g_offPlayerList = 0;   // Game -> player entity pointer array  (0xc0)
uint32_t g_offPlayerIndex = 0;  // Game -> local player index (int)     (0x168)
// Level map (Tab / X). It is the same camera controller in "map" mode (7): the
// game puts the camera high above a target point, looking straight down, with a
// fixed 16:9 field of view of 90 degrees. In VR the map is shown as a 3D scale
// model instead: the camera keeps the head's rotation and the VR field of view,
// looks at the target from above and behind, and the eye separation and head
// motion are scaled up so the level looks like a model on a table.
uint32_t g_offCamPrevMode = 0;  // camObj -> previous mode (mode itself at +0)  (4)
uint32_t g_offCamBlend = 0;     // camObj -> mode transition 0..1              (0x25c)
uint32_t g_offMapTarget = 0;    // camObj -> point looked at (vec3)            (0x128)
uint32_t g_offMapDist = 0;      // camObj -> zoom distance                     (0x188)
uint32_t g_offMapPos = 0;       // camObj -> smoothed map camera position      (0x110)
bool g_mapReady = false;
constexpr int kMapMode = 7;
// Camera mode while a script sets the camera (SetCameraTransform, read from the
// game's camera update at start; 8 in Teardown 2.0.1). 0 = not recognised.
int g_scriptCamMode = 0;
constexpr float kMapElevation = 50.0f * 3.14159265f / 180.0f;  // view angle below the horizon
constexpr float kMapTableDist = 1.2f;  // the level looks like a model this far away (m)
struct MapView
{
    bool on = false;
    Vec3 from;          // camera position when the map opened
    Vec3 eyeRef;        // head position (room) when the map opened
    Vec3 last;          // last map camera position (for the closing transition)
    float yaw = 0;      // world yaw the map is seen from (radians)
    float lastK = 1;
    float anchor = 0;   // room yaw (degrees) at the previous map frame
} g_map;
Vec3 g_leftEyePos;              // headset space (recentred), metres
Vec3 g_lastViewPos;             // left-eye camera position of the last normal frame
float g_vrFov = 0;              // field of view passed to SetCameraFov (engine convention)
// Off-centre projection per eye (see EngineSetEyeProjection): the engine builds
// every matrix from the camera's 4x4 projection (culling, shaders' view rays
// through mubProjectionData, TAA jitter added on top), so its m0/m5/m8/m9 can be
// replaced to render exactly the headset's asymmetric field of view.
float g_eyeTan[2][4] = {};
bool g_offCenter = false;

void ApplyEyeProjection(float* proj, int eye)
{
    const float* t = g_eyeTan[eye];
    float h = t[0] + t[1], v = t[2] + t[3];
    if (h <= 0.01f || v <= 0.01f) return;
    proj[0] = 2.0f / h;               // m0
    proj[8] = (t[1] - t[0]) / h;      // m8: horizontal shift (right minus left)
    proj[5] = 2.0f / v;               // m5
    proj[9] = (t[2] - t[3]) / v;      // m9: vertical shift (up minus down)
}
float g_curK = 1;               // eye separation scale of the current stereo pair
float g_mapPan[2] = {}, g_mapZoom = 0;

Pose g_eyeOffset{ {}, { 0.064f, 0, 0 } };
Pose g_leftCam;
bool g_leftCamValid = false;
bool g_leftCamRoom = false;     // g_leftCam is the VR head's camera in the room (mods' menu panels)
Quat g_leftEyeRot;              // headset space
float g_anchorYaw = 0;          // degrees
uint64_t g_anchorFrame = 0, g_frameCounter = 0;
uint64_t g_normalUpdates = 0;   // game updates that were not a frozen right-eye frame (dual wield: DualEye)
double* g_timeProbe = nullptr;

constexpr float kMaxAccumulated = 2.99f / 60.0f;  // same clamp as the engine
constexpr float kFrozenDt = 1e-5f;                // "zero" but never divides by zero

// Stereo state (main thread only).
bool g_leftThisFrame = false;   // the Lua mod rendered the left eye this frame
bool g_rightPending = false;    // next frame is a frozen right-eye frame
bool g_inFrozenUpdate = false;  // currently inside the frozen frame's update
bool g_rightThisFrame = false;  // this frame renders the right eye
float g_skippedDt = 0;
float g_vrFrameDelta = 0;      // headset display interval for the next left-eye frame (0 = none)
bool g_lastWasFrozen = false;  // the previous engine frame was a right-eye frame
int g_frozenFrames = 0;
float g_lastLeftDt = 0;        // frame time of the last left-eye frame
uint64_t g_catchUpDrops = 0;   // physics catch-up steps dropped (limitCatchUp)
// Diagnostics: Lua requests seen during the current update.
bool g_inUpdate = false;
int g_requestsThisUpdate = 0;
int g_anomalies = 0;
uint64_t g_updates = 0;

void CheckRequests(bool frozen, float dt)
{
    static int prev = 0;
    ++g_updates;
    bool odd = g_requestsThisUpdate >= 2 || (g_requestsThisUpdate == 0 && prev == 1);
    prev = g_requestsThisUpdate;
    if (odd && Cfg().debugLua && g_anomalies < 60) {
        ++g_anomalies;
        Log("Engine diag: update #%llu (%s, dt=%.5f) saw %d Lua requests", (unsigned long long)g_updates,
            frozen ? "frozen" : "normal", dt, g_requestsThisUpdate);
    }
    g_requestsThisUpdate = 0;
}

BYTE* ScanIn(BYTE* start, size_t size, const char* pattern)
{
    std::vector<int> pat;
    for (const char* p = pattern; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        pat.push_back((int)strtol(p, nullptr, 16));
        p += 2;
    }
    size_t n = pat.size();
    for (size_t i = 0; i + n <= size; ++i) {
        size_t j = 0;
        for (; j < n; ++j)
            if (pat[j] >= 0 && start[i + j] != pat[j]) break;
        if (j == n) return start + i;
    }
    return nullptr;
}

BYTE* Scan(const char* pattern)
{
    std::vector<int> pat;
    for (const char* p = pattern; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        pat.push_back((int)strtol(p, nullptr, 16));
        p += 2;
    }
    size_t n = pat.size();
    for (size_t i = 0; i + n <= g_textSize; ++i) {
        size_t j = 0;
        for (; j < n; ++j)
            if (pat[j] >= 0 && g_text[i + j] != pat[j]) break;
        if (j == n) return g_text + i;
    }
    return nullptr;
}

BYTE* RelTarget(BYTE* instrEnd, BYTE* rel32Pos)
{
    int32_t d;
    memcpy(&d, rel32Pos, 4);
    return instrEnd + d;
}

uint32_t U32(BYTE* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// 0 = no map, 1 = map open (or opening), 2 = map closing. blend = transition 0..1.
int MapPhase(BYTE* camObj, float& blend)
{
    if (!g_mapReady || !camObj) return 0;
    int mode = *(int*)camObj, prev = *(int*)(camObj + g_offCamPrevMode);
    blend = std::clamp(*(float*)(camObj + g_offCamBlend), 0.0f, 1.0f);
    if (mode == kMapMode) return 1;
    if (prev == kMapMode && blend < 1.0f && g_map.on) return 2;
    return 0;
}

// Joysticks on the map: left stick moves the point looked at (in the direction
// the head faces), right stick up/down zooms. The game clamps both itself.
void MapControls(BYTE* game, float dt)
{
    if (!g_mapReady || !g_map.on || !g_vrActive || dt <= 0) return;
    BYTE* camObj = *(BYTE**)(game + g_offGamePlayer);
    float blend;
    if (!camObj || MapPhase(camObj, blend) != 1) return;
    float* dist = (float*)(camObj + g_offMapDist);
    Vec3* target = (Vec3*)(camObj + g_offMapTarget);
    float facing = g_anchorYaw * 3.14159265f / 180.0f + YawOf(g_leftEyeRot);
    Quat q = AxisAngle({ 0, 1, 0 }, facing);
    Vec3 fwd = Rotate(q, { 0, 0, -1 }), right = Rotate(q, { 1, 0, 0 });
    float speed = (*dist + 10.0f) * 0.8f * dt;
    *target = *target + (right * g_mapPan[0] + fwd * g_mapPan[1]) * speed;
    if (g_mapZoom != 0) *dist = std::max(1.0f, *dist * std::exp(-g_mapZoom * 1.5f * dt));
}

// Dual wield, end of a game update: an overridden shot whose script asked for fewer eyes than its
// profile (2 for Total Overdose: a mod update that no longer computes both cannons?) would put its
// bullet somewhere else - after 3 such shots in a row dual wield is turned off for that tool for the
// session. (One alone: another eye request of the mod in the update after a click it did not fire -
// its dash - taken for a shot. More calls are fine: a dash in the same tick asks again; only the first
// ones are overridden.)
void DualShotLog();   // (with the eye hook, below)

void DualFrameEnd()
{
    DualState& D = g_dual;
    // (the first normal update after our click reached the game just ended, and the tool's script asked
    // for no eye we took: no shot - an object held, the tool not usable then, dual wield not allowed...;
    // with "late eye" lines below: the game reads the press one update later than expected)
    if (D.downId && !g_lastWasFrozen && g_normalUpdates == D.downNormal + 1 && D.usedId != D.downId) {
        static int logs = 0;
        if (logs < 20 || (Cfg().debugLua && logs < 300)) {
            ++logs;
            Log("Engine: dual: click %u reached the game, but no eye of the tool's script in the next update was taken "
                "for its shot (it did not fire: an object held, the tool not usable then, dual wield not allowed...)",
                D.downId);
        }
    }
    if (D.shotFrame != g_frameCounter) return;
    DualShotLog();
    if (!D.shotOk || !D.prof) return;
    if (D.calls >= D.prof->calls) {
        D.shortShots = 0;
        return;
    }
    const int i = (int)(D.prof - kDualProfiles);
    if (i < 0 || i >= kDualProfileCount || D.off[i]) return;
    if (++D.shortShots < 3) {
        Log("Engine: dual: shot #%d of '%s' asked for %d eye(s) instead of %d (%d in a row) - another eye request of the "
            "mod taken for a shot? Dual wield turns off for this tool after 3 in a row", D.shots, D.prof->tool, D.calls,
            D.prof->calls, D.shortShots);
        return;
    }
    D.off[i] = true;
    Log("Engine: dual: 3 shots in a row of '%s' asked for %d eye(s) instead of %d - the mod changed? Dual wield OFF for "
        "this tool until the game restarts (its two guns are held and fired as before)", D.prof->tool, D.calls,
        D.prof->calls);
}

// perfTrace: end of Game::update (the fixed-step loop starts: P_FixedLoop runs until
// ParticleSystem::update, see Hook_PsUpdate) and the physics step counter then.
double g_perfUpdExit = 0;
int g_perfStepAtExit = INT_MIN;
// [Performance] frameBMode (frameb.h): the module is on (its hooks installed, InitPerfTrace), and the
// census hooks of perf.cpp need Perf::UpdateBegin at every update.
bool g_frameBOn = false;
bool g_perfCensus = false;

void PerfUpdateExit(BYTE* g)
{
    g_perfUpdExit = ProfNow();
    g_perfStepAtExit = g_gameStep ? *(const int*)(g + g_gameStep) : INT_MIN;
}

// Engine frame of the last render-only frame B (frameBMode 2 replayed it instead of Game::update: nothing
// moved since frame A; skipShapeJobsB, Hook_ShapeJobs). 0 = none.
uint64_t g_frameBRenderOnly = 0;
// [Affichage] stereoParticleMotion (Hook_MeshTask, on the job worker threads): this engine frame's number, for the
// particle mesh tasks' check that what Hook_PsUpdate decided for them is this frame's (a task still running after the
// next frame started does nothing of ours).
std::atomic<uint64_t> g_pmFrameNow{ 0 };

// grabKeepDistance: the game's grab held where it was taken, moved with the stick (below LocalPlayer).
void GrabControls(float dt);
// pauseWide: a paused level's frame drawn for a 16:9 screen, decided at the end of a frame A's update (WideState).
void WideDecide(BYTE* g);

void __fastcall Hook_Update(void* game)
{
    BYTE* g = (BYTE*)game;
    g_gameObj = g;
    if (g_perfTrace || g_perfCensus) Perf::UpdateBegin(g_rightPending);
    DualFrameEnd();
    ++g_frameCounter;
    g_pmFrameNow.store(g_frameCounter, std::memory_order_relaxed);   // (stereoParticleMotion: the mesh tasks' check)
    if (Cfg().debugProfile && g_frameCounter == 1) SamplerStart();
    if (Cfg().debugLua) {
        // Game time vs real time (the game must not run faster than reality).
        static double realStart = 0, gameSum = 0;
        LARGE_INTEGER c, f;
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        double now = (double)c.QuadPart / f.QuadPart;
        if (realStart == 0) realStart = now;
        if (now - realStart > 5.0) {
            Log("Engine: game time %.2f s for %.2f s of real time (x%.2f)", gameSum, now - realStart, gameSum / (now - realStart));
            realStart = now;
            gameSum = 0;
        }
        g_timeProbe = &gameSum;
    }
    float* dt = (float*)(g + g_offDt);
    float* acc1 = (float*)(g + g_offAcc1);
    float* acc2 = (float*)(g + g_offAcc2);

    // Game::frame adds the frame time to the fixed-step accumulators BEFORE this
    // update, and runs the fixed-step (physics) loop right AFTER it (0x7c5c0,
    // "while (acc >= step) { step(); acc -= step; } alpha = acc / step").
    const float realDt = *dt;
    if (g_rightPending) {
        // Frame B: give this frame no time at all. Taking its time back out of the
        // accumulators makes the following physics loop run no step, and keeps
        // the render interpolation (alpha) exactly as in frame A.
        g_rightPending = false;
        g_skippedDt += realDt;
        *acc1 = std::max(0.0f, *acc1 - realDt);
        *acc2 = std::max(0.0f, *acc2 - realDt);
        *dt = kFrozenDt;
        g_inFrozenUpdate = true;
        g_inUpdate = true;
        bool replayed = false;
        {
            ProfScope prof(P_Update);
            // frameBMode 2: no Game::update here, frame A's per-frame requests made again instead
            // (frameb.h; false: not possible for this pair, the full update as before). Our Lua mod
            // is then not ticked in this frame: its laser, outlines and sprites are frame A's,
            // replayed; the right-eye camera is Hook_CamSet's as always.
            replayed = g_frameBOn && FrameB::BReplay(g, g_frameCounter);
            if (!replayed) Real_Update(game);
        }
        g_inUpdate = false;
        g_inFrozenUpdate = false;
        if (!replayed) CheckRequests(true, kFrozenDt);   // (replayed: no Lua request in this frame, by design)
        g_frameBRenderOnly = replayed ? g_frameCounter : 0;
        g_rightThisFrame = true;
        g_lastWasFrozen = true;
        if (++g_frozenFrames == 1) Log("Engine: first synchronised right-eye frame");
        if (g_perfTrace) PerfUpdateExit(g);
        return;
    }

    if (g_lastWasFrozen && g_vrFrameDelta > 0 && g_vrActive) {
        // Left-eye frame right after a stereo pair: advance the game by exactly the
        // headset's display interval instead of this frame's measured time (frame
        // B's time was already taken out).
        float d = std::min(g_vrFrameDelta, 0.1f);
        *dt = d;
        // After a late frame the display interval covers two or more headset frames, and
        // the engine would run two physics steps to catch up - making this frame slow
        // too, so hitches chain up during heavy explosions. With limitCatchUp at most
        // one step runs per frame when the headset runs faster than the physics
        // (the time beyond that is dropped: a very short slow-down instead).
        float maxAcc = kMaxAccumulated;
        if (Cfg().limitCatchUp) maxAcc = std::min(kMaxAccumulated, std::max(1.98f / 60.0f, d + 0.98f / 60.0f));
        float raw = *acc1 - realDt + d;
        if (raw > maxAcc && raw <= kMaxAccumulated + d) ++g_catchUpDrops;
        *acc1 = std::clamp(raw, 0.0f, maxAcc);
        *acc2 = std::clamp(*acc2 - realDt + d, 0.0f, maxAcc);
        g_skippedDt = 0;
    } else if (g_skippedDt > 0) {
        // Frame A after a frozen frame: simulate the frozen frame's time now.
        *dt += g_skippedDt;
        *acc1 = std::min(*acc1 + g_skippedDt, kMaxAccumulated);
        *acc2 = std::min(*acc2 + g_skippedDt, kMaxAccumulated);
        g_skippedDt = 0;
    }
    g_lastWasFrozen = false;
    ++g_normalUpdates;
    g_vrFrameDelta = 0;
    float frameDt = *dt;
    g_lastLeftDt = frameDt;
    MapControls(g, frameDt);
    GrabControls(frameDt);   // (before Game::update: the player's update clamps the grab's wanted distance and follows it)
    if (g_timeProbe) *g_timeProbe += *dt;
    g_inUpdate = true;
    // frameBMode: the pair's mode is decided here; in mode 2 what this update asks for one frame
    // only is recorded for the right-eye frame B (frameb.h).
    if (g_frameBOn) FrameB::AEnter(g, g_frameCounter, g_vrActive && g_ready && Cfg().stereoMode == 0);
    // ([Performance] jobSpin: frame A's job phases start - idle job threads and Job::wait may spin, perf.h)
    Perf::SpinFrameA(g_vrActive && g_ready && Cfg().stereoMode == 0);
    {
        ProfScope prof(P_Update);
        Real_Update(game);
    }
    if (g_frameBOn) FrameB::AExit(g);
    g_inUpdate = false;
    // (pauseWide - before CheckRequests: it needs this update's count of Lua eye requests, none in a paused
    // single-player level)
    WideDecide(g);
    CheckRequests(false, frameDt);
    if (g_perfTrace) PerfUpdateExit(g);
}

// The Lua mod passes the pose of the VR hand (in the tool frame) through
// SetToolTransformOverride. The engine has just computed the normal first-person
// pose of the tool, animations included (swing, recoil...). We keep that pose but
// make it relative to the VR hand instead of the default hand position, so every
// tool is held the way the game shows it - and still animates.
Pose ReadPose(const BYTE* p)
{
    const float* f = (const float*)p;
    return { { f[3], f[4], f[5], f[6] }, { f[0], f[1], f[2] } };
}

void WritePose(BYTE* p, const Pose& v)
{
    float* f = (float*)p;
    f[0] = v.p.x; f[1] = v.p.y; f[2] = v.p.z;
    f[3] = v.q.x; f[4] = v.q.y; f[5] = v.q.z; f[6] = v.q.w;
}

// Rest pose of a built-in tool in the normal view (eye space) and the point of
// the tool held by the hand, read from the tool's prefab (data/prefab/tool/*.xml).
struct ToolRest
{
    bool known = false;
    Pose rest;   // "fp_action" location: the tool at rest, animations excluded
    Vec3 grip;   // "tp_action_rh" location: where the right hand holds it (tool space)
};

// Teardown's QuatEuler (degrees): Ry * Rz * Rx.
Quat EulerDeg(float x, float y, float z)
{
    const float k = 3.14159265f / 180.0f;
    return AxisAngle({ 0, 1, 0 }, y * k) * AxisAngle({ 0, 0, 1 }, z * k) * AxisAngle({ 1, 0, 0 }, x * k);
}

bool ReadAttr3(const std::string& tag, const char* attr, float v[3])
{
    size_t a = tag.find(std::string(" ") + attr + "=\"");
    if (a == std::string::npos) return false;
    std::istringstream in(tag.substr(a + strlen(attr) + 3));
    return (bool)(in >> v[0] >> v[1] >> v[2]);
}

bool FindLocation(const std::string& xml, const char* name, float pos[3], float rot[3])
{
    size_t at = xml.find(std::string("name=\"") + name + "\"");
    if (at == std::string::npos) return false;
    size_t b = xml.rfind('<', at), e = xml.find('>', at);
    if (b == std::string::npos || e == std::string::npos) return false;
    std::string tag = xml.substr(b, e - b);
    rot[0] = rot[1] = rot[2] = 0;
    ReadAttr3(tag, "rot", rot);
    return ReadAttr3(tag, "pos", pos);
}

const ToolRest& RestPoseOf(const std::string& name)
{
    static std::map<std::string, ToolRest> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    ToolRest& r = cache[name];
    // Built-in tools have lower-case names. A mod's tool (any id) must not pick up a
    // built-in prefab through the case-insensitive file system ("Gun" -> gun.xml).
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) return r;
    if (name == "sledge") {
        // No fp_action: the hammer's pose at rest is computed by its swing code
        // (Sledge::update 0x450690). With it removed, the handle runs along the
        // hand's -Z axis (through the fist) and the hand holds it at tp_action_rh,
        // the head 42.5 cm further: the head hits where it is seen (physical
        // hammer in main.lua, which uses the same geometry).
        r.known = true;
        r.rest = { { 0.1648f, -0.2166f, -0.1185f, 0.9549f }, { 0.2f, -0.45f, -0.35f } };
        r.rest.q = Normalize(r.rest.q);
        r.grip = { -0.025f, -0.025f, 0.05f };   // on the handle, 0.725 m from the head centre
        return r;
    }
    std::wstring path = ModDirectory() + L"data\\prefab\\tool\\" + std::wstring(name.begin(), name.end()) + L".xml";
    std::ifstream f(path, std::ios::binary);
    if (!f) return r;   // a tool added by a mod
    std::stringstream ss;
    ss << f.rdbuf();
    std::string xml = ss.str();
    float p[3], q[3];
    if (!FindLocation(xml, "fp_action", p, q)) return r;
    r.known = true;
    r.rest = { EulerDeg(q[0], q[1], q[2]), { p[0], p[1], p[2] } };
    Log("Engine: tool '%s' rest (%.3f %.3f %.3f) rot (%.1f %.1f %.1f)", name.c_str(), p[0], p[1], p[2], q[0], q[1], q[2]);
    if (FindLocation(xml, "tp_action_rh", p, q)) r.grip = { p[0], p[1], p[2] };
    return r;
}

// Name of the tool in hand (built-in tools: "gun", "sledge"...; tools added by mods:
// their id as registered, e.g. "GPD", "BD_38Revolver"), or "" if unreadable.
// The engine's string is 32 bytes: the characters inline while its last byte is 0,
// otherwise a heap pointer (ids of 32 characters or more: not read, old behaviour).
std::string CurrentToolName(BYTE* toolAnim)
{
    if (!g_offToolEntry) return {};
    BYTE* entry = *(BYTE**)(toolAnim + g_offToolEntry);
    if (!entry) return {};
    const char* s = (const char*)(entry + g_offToolName);
    if (s[31] != 0) return {};
    std::string name;
    for (int i = 0; i < 31 && s[i]; ++i) {
        char c = s[i];
        if (c <= ' ' || c > '~') return {};   // (also rejects bytes >= 0x80: char is signed)
        name += c;
    }
    return name;
}

// The local player entity (the one the VR player controls), or null.
BYTE* LocalPlayer()
{
    if (!g_gameObj || !g_offPlayerList) return nullptr;
    BYTE** list = *(BYTE***)(g_gameObj + g_offPlayerList);
    int index = *(int*)(g_gameObj + g_offPlayerIndex);
    if (!list || index < 0 || index > 64) return nullptr;
    return list[index];
}

// ---------------------------------------------------------------------------
// The game's grab (dominant grip) held where it was taken (grabKeepDistance)
// ---------------------------------------------------------------------------
// Read in teardown.exe 2.1 (docs/REVERSE_ENGINEERING.md, 7 ter). Player::grab (+ba130) takes the object at
// its pick distance d0 (from the player's camera: the eyes) and writes the distance now (+7ACh) = d0 and the
// wanted distance (+7B0h) = max(1.5, d0 - 0.2). Then every Player::update (+c2360) while it holds something
// (+788h) adds the mouse wheel to the wanted distance (0.25 m a notch) and clamps it to [1.5, 3]; the grab's
// update (+c56b0, called by it afterwards) moves the distance half-way to it each frame (0.5 m at most); each
// physics step pulls the object to the camera + its forward axis x that distance (+c5080). So an object taken
// closer than 1.7 m was pushed out to 1.5 m, and any other one moved 20 cm away from where it was taken.
// InitGrabDistance makes three instructions read those constants (0.2 and the two 1.5) from g_grabConst
// instead (their rip-relative operands redirected; nothing else of the game's code changes): [0] what the
// grab takes off d0, [1] the shortest wanted distance. While VR runs with grabKeepDistance: 0 and
// grabMinDistance - the object stays where it was taken, at least that far from the eyes (closer, it would
// reach into the player); otherwise 0.2 and 1.5, the game's own rule. GrabControls (each frame A, before
// Game::update) also moves the wanted distance with the dominant stick (Input, GrabStick: m/s); the game
// clamps it to [min, 3] and eases to it as it does with its wheel. Picking, force, throw and release stay
// the game's.
uint32_t g_offGrabBody = 0, g_offGrabWant = 0, g_offGrabDist = 0;   // player+788h, +7B0h, +7ACh (2.1)
float* g_grabConst = nullptr;                // read by the game's code (AllocNearGame)
float g_grabMax = 3.0f;                      // the game's longest wanted distance (its own operand)
bool g_grabOk = false;
std::atomic<float> g_grabStickVel{ 0.0f };   // m/s, + = farther (EngineSetGrabStick)

void GrabControls(float dt)
{
    if (!g_grabOk) return;
    const Settings& c = Cfg();
    const bool keep = c.grabKeepDistance && g_vrActive;
    g_grabConst[0] = keep ? 0.0f : 0.2f;
    g_grabConst[1] = keep ? std::clamp(c.grabMinDistance, 0.2f, 1.5f) : 1.5f;
    BYTE* pl = g_vrActive ? LocalPlayer() : nullptr;
    const bool holding = pl && *(void**)(pl + g_offGrabBody) != nullptr;
    float* want = holding ? (float*)(pl + g_offGrabWant) : nullptr;
    static bool held = false, moving = false;
    static int logs = 0;
    // (the grab ran in the last update: its wanted distance is where the object was taken, or the minimum)
    if (holding && !held && logs < 60) {
        ++logs;
        Log("Engine: game grab - object taken, held at %.2f m from the eyes (distance now %.2f m; min %.2f, max %.1f; "
            "grabKeepDistance %d)", *want, *(float*)(pl + g_offGrabDist), g_grabConst[1], g_grabMax, keep ? 1 : 0);
    }
    const float v = keep && holding ? g_grabStickVel.load() : 0.0f;
    if (v != 0 && dt > 0) *want = std::clamp(*want + v * c.worldScale * dt, g_grabConst[1], g_grabMax);
    if (moving && v == 0 && holding && logs < 60) {
        ++logs;
        Log("Engine: game grab - moved with the stick, held at %.2f m from the eyes", *want);
    }
    moving = v != 0;
    held = holding;
}

// Dual wield: the mod's desktop pose of its tool this frame (SetToolTransform). Total Overdose kicks
// it up for the frame it fires (QuatEuler(30) instead of -10): for a shot of the off hand's gun the
// tool in the dominant hand must not kick, so while such a click is in flight and delivered, a pose
// different from the last one seen without a shot is replaced by that one (the Lua mod kicks the copy
// itself). The pose is kept from frames with no click in flight, or after a click's shot frame.
void DualToolPose(Pose& t)
{
    DualState& D = g_dual;
    if (!D.prof || g_frameCounter - D.profFrame > 2) return;
    const int gun = g_dualClickGun.load(std::memory_order_acquire);
    const bool delivered = gun >= 0 && D.downId == g_dualClickId.load(std::memory_order_acquire) &&
                           g_frameCounter > D.downFrame;
    if (delivered && gun == 0) {
        if (D.restOk && memcmp(&t, &D.restT, sizeof(Pose)) != 0) {
            t = D.restT;
            ++D.kicksHeld;
        }
        return;
    }
    if (gun < 0 || (delivered && g_frameCounter > D.downFrame + 1)) {
        D.restT = t;
        D.restOk = true;
    }
}

// Lua binding of SetToolTransform(transform, sway, playerId). In VR, when our Lua
// mod's override (the VR hand) is already in the shared field, the mod's transform
// is kept aside and the hand put back; when the mod runs first, its transform is
// simply read (our override replaces it later in the frame).
__int64 __fastcall Hook_SetToolTransform(void* ctx, void* args, void* a3, void* a4)
{
    BYTE* pl = g_vrActive ? LocalPlayer() : nullptr;
    if (!pl || !g_offToolOverride || !g_offToolFlag) return Real_SetToolTransform(ctx, args, a3, a4);
    BYTE* slot = pl + g_offToolOverride;
    BYTE before[28];
    memcpy(before, slot, sizeof(before));
    bool handThere = pl[g_offToolFlag] != 0;
    __int64 r = Real_SetToolTransform(ctx, args, a3, a4);
    // (unchanged while the hand is there: the call was for another player)
    if (!handThere || memcmp(before, slot, sizeof(before)) != 0) {
        Pose t = ReadPose(slot);
        if (std::isfinite(t.p.x) && std::isfinite(t.p.y) && std::isfinite(t.p.z) && std::isfinite(t.q.x) &&
            std::isfinite(t.q.y) && std::isfinite(t.q.z) && std::isfinite(t.q.w)) {
            DualToolPose(t);   // (an off-hand shot does not kick the dominant gun)
            g_scriptToolT = t;
            g_scriptToolTSet = true;
            g_toolScriptCtx = ctx;
            g_toolScriptFrame = g_frameCounter;
        }
        if (handThere) memcpy(slot, before, sizeof(before));
    }
    return r;
}

// Desktop pose of a tool added by a mod: the transform its script sets this frame
// with SetToolTransform (eye space; identity when it sets none, like the engine),
// approached exponentially at the engine's own rate (60/s, script-tool animator
// 0x451de0), so the mod's animations keep their look and timing. The engine's
// walking sway is left out: in VR the tool follows the hand.
Pose ScriptToolFlat(const std::string& name, const Pose& target, float dt)
{
    static std::unordered_map<std::string, bool> logged;
    Pose& flat = g_scriptToolFlat;
    Quat t = Normalize(target.q);
    if (name != g_scriptToolName) {
        g_scriptToolName = name;   // (cleared while another kind of tool is held)
        flat = { t, target.p };
        if (!logged[name]) {
            logged[name] = true;
            Log("Engine: tool '%s' comes from a mod - held in the hand from its desktop pose (%.2f %.2f %.2f)",
                name.c_str(), target.p.x, target.p.y, target.p.z);
        }
        return flat;
    }
    float a = 1.0f - std::exp(-60.0f * std::clamp(dt, 0.0f, 0.1f));
    flat.p = flat.p + (target.p - flat.p) * a;
    if (flat.q.x * t.x + flat.q.y * t.y + flat.q.z * t.z + flat.q.w * t.w < 0) t = { -t.x, -t.y, -t.z, -t.w };
    flat.q = Normalize({ flat.q.x + (t.x - flat.q.x) * a, flat.q.y + (t.y - flat.q.y) * a,
                         flat.q.z + (t.z - flat.q.z) * a, flat.q.w + (t.w - flat.q.w) * a });
    return flat;
}

// Size of tools added by mods. Mods draw their tools for a desktop screen, where the
// tool sits 0.6-2.5 m in front of the camera: held in the VR hand (about 0.45 m from
// the eyes) they looked huge (a revolver 1.5 m long, a chaingun 3.8 m). Each mod tool
// is scaled so that it looks as big in the hand as it does on a desktop screen
// (0.45 m / its desktop distance), at most 1.1 m long, times the player's setting.
// Teardown shapes own their voxels (they are destructible), so the voxel size of the
// tool's shapes can be changed without touching anything else; their positions in
// the tool body are scaled the same way. A script that moves its own shapes (e.g. an
// animated barrel) keeps working: a position it writes becomes the new original, and
// is scaled as soon as it is written (SetShapeLocalTransform hook), so every script -
// our Lua mod measuring the tool for its physical copy included - sees the tool as
// it is drawn.
constexpr uint32_t kPlayerToolBody = 0x820;   // player entity -> tool body (entity type 1)
constexpr uint32_t kEntityType = 0x08, kEntitySibling = 0x18, kEntityChild = 0x28;
constexpr uint32_t kShapeLocal = 0x44;        // shape -> local transform (pos, quat)
constexpr uint32_t kShapeVox = 0xf0;          // shape -> voxel data (int sizes[3] at +0, float scale at +0x20)
constexpr float kVrHandDist = 0.45f;          // eyes to the VR hand (m)
// Point of the held mod tool that its scaling keeps in place, in its body: its muzzle
// (ToolMuzzle, ModToolPivot). Its shapes are drawn at m + s (p - m).
Vec3 g_scaledPivot;

constexpr uint32_t kEntityParent = 0x10;      // entity -> parent entity (a shape's body)
constexpr int kMaxToolShapes = 4096;          // (THE EVERYTHING REVOLVER: 1028 shapes - a limit of 64 left most unscaled)
constexpr float kMinVoxel = 1e-5f;            // smallest voxel size scaled (the revolver's engravings: 0.35 mm)
constexpr float kHiddenDist = 20.0f;          // a shape farther from the tool body is hidden by its mod (not measured)

struct ScaledShape { Vec3 origPos, wrotePos; float origScale = 0, wroteScale = 0; uint32_t seen = 0; };
uint32_t g_scalePass = 0;        // ApplyToolScale pass over the whole tool (a record not seen in the last one: deleted shape)
int g_scaledShapes = 0;          // shapes of g_scaledBody in the last pass
std::unordered_map<BYTE*, ScaledShape> g_scaled;   // shapes of the current mod tool
BYTE* g_scaledBody = nullptr;
float g_scaledS = 1.0f;          // factor applied to g_scaledBody
std::unordered_map<std::string, float> g_autoScale;  // per tool name (desktop look -> hand)
std::unordered_map<std::string, int> g_autoShapes;   // shape count of each measured tool
// The tool being measured (ToolSettled): its shapes and look must stay the same for a moment.
struct ToolSettle { BYTE* body = nullptr; int shapes = -1; float len = 0, dist = 0, shown = 1.0f, stable = 0, total = 0; } g_settle;
PFN_LuaFn Real_SetShapeLocal = nullptr;

void ApplyToolScale(BYTE* body, float s, BYTE* only = nullptr);

// Calls f for every shape of the tool body; returns their number.
template<class F> int ForToolShapes(BYTE* body, F f)
{
    int shapes = 0;
    BYTE* c = *(BYTE**)(body + kEntityChild);
    for (int n = 0; c && n < kMaxToolShapes; ++n, c = *(BYTE**)(c + kEntitySibling))
        if (*(uint16_t*)(c + kEntityType) == 2 && *(BYTE**)(c + kShapeVox)) {
            f(c);
            ++shapes;
        }
    return shapes;
}

// Position and voxel size of a tool shape as its mod made it (a value the mod wrote
// since our last write is its new original).
void ShapeOriginal(BYTE* sh, Vec3& pos, float& scale)
{
    const float* lt = (const float*)(sh + kShapeLocal);
    pos = { lt[0], lt[1], lt[2] };
    scale = *(const float*)(*(BYTE**)(sh + kShapeVox) + 0x20);
    auto f = g_scaled.find(sh);
    if (f == g_scaled.end()) return;
    const ScaledShape& r = f->second;
    if (pos.x == r.wrotePos.x && pos.y == r.wrotePos.y && pos.z == r.wrotePos.z) pos = r.origPos;
    if (scale == r.wroteScale) scale = r.origScale;
}

// Factor for a mod tool from its unscaled shapes and desktop pose. Shapes its mod hides
// (at infinity or far away: THE EVERYTHING REVOLVER adds its parts at Transform(1/0)
// before placing them) are left out. False when nothing can be measured.
bool MeasureToolScale(BYTE* body, const Pose& flat, float& s, float& len, float& dist)
{
    Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
    int shapes = 0;
    ForToolShapes(body, [&](BYTE* sh) {
        Vec3 pos;
        float scale;
        ShapeOriginal(sh, pos, scale);
        const int* size = *(const int* const*)(sh + kShapeVox);
        const float* lt = (const float*)(sh + kShapeLocal);
        Quat q{ lt[3], lt[4], lt[5], lt[6] };
        if (!(scale > kMinVoxel && scale < 1.0f) || size[0] <= 0 || size[1] <= 0 || size[2] <= 0) return;
        if (!(Length(pos) < kHiddenDist)) return;   // (infinite and NaN positions too)
        for (int k = 0; k < 8; ++k) {
            Vec3 c{ (k & 1) ? size[0] * scale : 0.0f, (k & 2) ? size[1] * scale : 0.0f, (k & 4) ? size[2] * scale : 0.0f };
            Vec3 p = pos + Rotate(q, c);
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        }
        ++shapes;
    });
    if (!shapes || !(hi.x >= lo.x && hi.y >= lo.y && hi.z >= lo.z)) return false;
    Vec3 centre = (lo + hi) * 0.5f;
    dist = Length(flat.p + Rotate(flat.q, centre));
    len = std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z });
    s = dist > 0.01f ? std::clamp(kVrHandDist / dist, 0.25f, 1.0f) : 1.0f;   // (no jump at any distance)
    if (len * s > 1.1f) s = std::max(0.2f, 1.1f / len);
    return std::isfinite(s);
}

// True once a mod tool has settled and can be measured. Mods build their tools over
// several frames and ease them into place (THE EVERYTHING REVOLVER replaces the shape
// the game gives it, then adds its 1028 shapes in two batches): its shape count, length
// and desktop distance must stay the same (within 3%) for 0.3 s, at most 3 s after it
// is first seen (the distance, not the factor: the factor stays at its bound while a
// far tool comes closer). now: the factor to apply meanwhile. It follows the shape
// count, but the look only when it changes the factor by more than 10%: a tool that
// sways or bobs keeps one size (each new size makes the Lua mod redo the grip and the
// physical copy). 1 while nothing can be measured.
bool ToolSettled(BYTE* body, int count, const Pose& flat, float dt, float& now)
{
    ToolSettle& w = g_settle;
    if (w.body != body) w = ToolSettle{ body };
    float step = std::clamp(dt, 0.0f, 0.1f), m = 1.0f, len = 0, dist = 0;
    w.total += step;
    bool ok = MeasureToolScale(body, flat, m, len, dist);
    if (!ok) m = 1.0f;
    if (!ok || count != w.shapes || !(std::abs(len - w.len) <= 0.03f * w.len) ||
        !(std::abs(dist - w.dist) <= 0.03f * w.dist)) {
        if (!ok || count != w.shapes || !(std::abs(m - w.shown) <= 0.1f * w.shown)) w.shown = m;
        w.shapes = ok ? count : -1;
        w.len = len;
        w.dist = dist;
        w.stable = 0;
    } else {
        w.stable += step;
    }
    now = w.shown;
    return w.stable >= 0.3f || w.total >= 3.0f;
}

// Automatic factor for a mod tool once it has settled (ToolSettled): the factor it was
// given while settling when its look confirms it (within 3%, or 10% for a tool that
// never kept still), so that it does not change size again at the end.
float AutoToolScale(const std::string& name, BYTE* body, const Pose& flat)
{
    float s = 1.0f, len = 0, dist = 0;
    int count = ForToolShapes(body, [](BYTE*) {});
    g_autoShapes[name] = count;
    if (!MeasureToolScale(body, flat, s, len, dist)) {
        Log("Engine: mod tool '%s' (%d shapes): nothing to measure - not scaled automatically", name.c_str(), count);
        return 1.0f;
    }
    const ToolSettle& w = g_settle;
    float tol = w.stable >= 0.3f ? 0.03f : 0.1f;
    if (w.body == body && w.shapes == count && std::abs(s - w.shown) <= tol * w.shown) s = w.shown;
    Log("Engine: mod tool '%s' %.2f m long (%d shapes), shown %.2f m in front on a desktop: scaled to %.0f%% in the hand",
        name.c_str(), len, count, dist, s * 100.0f);
    return s;
}

// SetShapeLocalTransform's engine function, hooked (Hook_ShapeLocalImpl, below; nullptr when not
// recognised).
using PFN_ShapeLocalImpl = __int64(__fastcall*)(void* ctx, BYTE* shape, const float* t, void* a4);
PFN_ShapeLocalImpl Real_ShapeLocalImpl = nullptr;

// Returns the factor applied (1 while the tool is not measured yet).
float ScaleModTool(BYTE* player, const std::string& name, const Pose& flat, const Pose* scriptT, float dt)
{
    BYTE* body = *(BYTE**)(player + kPlayerToolBody);
    if (!body || *(uint16_t*)(body + kEntityType) != 1) return 1.0f;
    static std::string scaledName;
    // Right-eye frame (fastToolScaleB): the same instant as the left-eye frame just before. With the
    // same tool (body, name, shape count, still the scaled one) its factor is that frame's, and its
    // whole scaling pass was done then: what its mod writes since (SetShapeLocalTransform) is scaled
    // one shape at a time by Hook_ShapeLocalImpl - when that hook is in place, or by the binding
    // hook's whole pass for a tool of 128 shapes at most. Settling is measured in left-eye frames
    // only (a frozen frame adds no time to it anyway).
    static struct { BYTE* body = nullptr; std::string name; int count = 0; uint64_t frame = 0; float s = 1.0f; } leftEye;
    if (g_inFrozenUpdate && Cfg().fastToolScaleB && leftEye.frame && leftEye.frame + 1 == g_frameCounter &&
        leftEye.body == body && body == g_scaledBody && leftEye.name == name && name == scaledName &&
        (Real_ShapeLocalImpl || (Real_SetShapeLocal && g_scaledShapes <= 128)) &&
        ForToolShapes(body, [](BYTE*) {}) == leftEye.count) {
        g_scaledS = leftEye.s;
        return leftEye.s;
    }
    if (body != g_scaledBody || name != scaledName) {   // another tool (or the same one re-created): new shapes
        g_scaled.clear();
        g_settle = {};   // (the engine may give the next tool's body the same address)
        g_scaledBody = body;
        scaledName = name;
    }
    int count = ForToolShapes(body, [](BYTE*) {});
    if (!count) return 1.0f;   // (shapes not created yet)
    // Measured from the mod's own desktop pose (this frame's SetToolTransform, else the
    // smoothed one) once the tool has settled, and scaled from its current look until
    // then. A measured tool whose shape count changes a lot (another skin) is measured
    // again once its new shapes have settled, keeping its factor meanwhile; a few shapes
    // more or less (a muzzle flash) change nothing.
    auto it = g_autoScale.find(name);
    auto known = g_autoShapes.find(name);
    bool rebuilt = it != g_autoScale.end() && known != g_autoShapes.end() &&
                   std::abs(count - known->second) > std::max(8, known->second / 2);
    float settling = 1.0f;
    bool settled = false;
    if (it == g_autoScale.end() || rebuilt)
        settled = ToolSettled(body, count, scriptT ? *scriptT : flat, dt, settling);
    else
        g_settle = {};
    if (rebuilt && settled) {
        Log("Engine: mod tool '%s' changed (%d shapes instead of %d): measured again", name.c_str(), count,
            known->second);
        g_autoScale.erase(it);
        it = g_autoScale.end();
    }
    if (it == g_autoScale.end() && settled) {
        it = g_autoScale.emplace(name, AutoToolScale(name, body, scriptT ? *scriptT : flat)).first;
    }
    float s = std::clamp((it != g_autoScale.end() ? it->second : settling) * std::clamp(Cfg().modToolScale, 0.3f, 1.5f),
                         0.15f, 1.5f);
    g_scaledS = s;
    ApplyToolScale(body, s);
    if (!g_inFrozenUpdate) {
        leftEye.body = body;
        leftEye.name = name;
        leftEye.count = count;
        leftEye.frame = g_frameCounter;
        leftEye.s = s;
    }
    return s;
}

// SetShapeLocalTransform, hooked in the engine function its binding calls once it has
// found the shape from its handle (also used by SetProperty): a mod animating a shape of
// its tool gives the unscaled position, so that shape alone is scaled right after. THE
// EVERYTHING REVOLVER moves about 125 of its 1028 shapes every tick: scaling the whole
// tool after each call (the binding hook below, when this function is not recognised)
// was far too slow. (Real_ShapeLocalImpl: above ScaleModTool.)
__int64 __fastcall Hook_ShapeLocalImpl(void* ctx, BYTE* shape, const float* t, void* a4)
{
    __int64 r = Real_ShapeLocalImpl(ctx, shape, t, a4);
    if (shape && g_scaledBody && g_scaledS != 1.0f && g_vrActive && *(BYTE**)(shape + kEntityParent) == g_scaledBody &&
        *(uint16_t*)(shape + kEntityType) == 2 && *(BYTE**)(shape + kShapeVox))
        if (BYTE* pl = LocalPlayer())
            if (*(BYTE**)(pl + kPlayerToolBody) == g_scaledBody) ApplyToolScale(g_scaledBody, g_scaledS, shape);
    return r;
}

// Lua binding of SetShapeLocalTransform: a mod animating a shape of its tool gives the
// unscaled position, scaled right after (the binding's arguments are not decoded: the
// shapes of the held tool whose position changed are the ones it moved).
__int64 __fastcall Hook_SetShapeLocal(void* ctx, void* args, void* a3, void* a4)
{
    __int64 r = Real_SetShapeLocal(ctx, args, a3, a4);
    if (g_scaledBody && g_scaledS != 1.0f && g_vrActive && g_scaledShapes <= 128)   // (whole tool: small ones only)
        if (BYTE* pl = LocalPlayer())
            if (*(BYTE**)(pl + kPlayerToolBody) == g_scaledBody) ApplyToolScale(g_scaledBody, g_scaledS);
    return r;
}

// Writes the scaled voxel size and position of every shape of the tool, or of the shape
// "only" (a position or size the mod changed since our last write becomes the new
// original). Records of shapes deleted since are forgotten once they clearly outnumber
// the tool's shapes.
void ApplyToolScale(BYTE* body, float s, BYTE* only)
{
    if (!only) ++g_scalePass;
    auto one = [&](BYTE* sh) {
        float* lt = (float*)(sh + kShapeLocal);
        float* scale = (float*)(*(BYTE**)(sh + kShapeVox) + 0x20);
        auto f = g_scaled.find(sh);
        if (f == g_scaled.end()) {
            f = g_scaled.emplace(sh, ScaledShape{ { lt[0], lt[1], lt[2] }, {}, *scale, 0 }).first;
        } else {
            // the mod moved or changed the shape itself since we last wrote it
            if (lt[0] != f->second.wrotePos.x || lt[1] != f->second.wrotePos.y || lt[2] != f->second.wrotePos.z)
                f->second.origPos = { lt[0], lt[1], lt[2] };
            if (*scale != f->second.wroteScale) f->second.origScale = *scale;
        }
        ScaledShape& r = f->second;
        r.seen = g_scalePass;
        if (!(r.origScale > kMinVoxel && r.origScale < 1.0f)) return;
        r.wrotePos = s == 1.0f ? r.origPos : g_scaledPivot + (r.origPos - g_scaledPivot) * s;   // (about the muzzle)
        r.wroteScale = r.origScale * s;
        lt[0] = r.wrotePos.x;
        lt[1] = r.wrotePos.y;
        lt[2] = r.wrotePos.z;
        *scale = r.wroteScale;
    };
    if (only) {
        one(only);
        return;
    }
    g_scaledShapes = ForToolShapes(body, one);
    if (g_scaled.size() > (size_t)g_scaledShapes * 2 + 64)
        for (auto it = g_scaled.begin(); it != g_scaled.end();) {
            if (it->second.seen != g_scalePass)
                it = g_scaled.erase(it);
            else
                ++it;
        }
}

// Muzzle of a mod tool, in its body: the centre of the solid voxels that reach the
// furthest forward in the mod's desktop view (eye -Z through its pose), within a thin
// slab, put on their front face. A barrel gives the centre of its mouth, a bunch of
// barrels (DOOM Chaingun) their middle. Mods start their shots, flashes, beams and
// lights from the tool as they made it, in its body: body transform x offset (GPD,
// Omnicutter, Vacuum, Z-Glock), animation bones (UMF: DOOM Chaingun, physgun; LnL:
// revolver, GLARE), prefab locations, a part x offset (Thermite Cannon). Scaled about
// this point, the drawn barrel ends exactly where they start; another point of the
// tool computed that way is off by (1 - s) x its distance to the muzzle.
// drawn: the shapes as drawn (diagnostics) instead of as the mod made them.
Vec3 ToolMuzzle(BYTE* body, const Pose& flat, bool drawn, int* voxels = nullptr)
{
    const Vec3 f = Rotate(Conjugate(Normalize(flat.q)), Vec3{ 0, 0, -1 });
    struct Grid { Vec3 o, ax, ay, az; int n[3]; const uint8_t* v; float d0, dx, dy, dz, vox; };
    std::vector<Grid> grids;
    long long budget = 16000000;   // voxels read at most (measured once per tool)
    ForToolShapes(body, [&](BYTE* sh) {
        auto it = drawn ? g_scaled.end() : g_scaled.find(sh);
        const float* lt = (const float*)(sh + kShapeLocal);
        BYTE* vd = *(BYTE**)(sh + kShapeVox);
        const int* size = (const int*)vd;
        const uint8_t* v = *(const uint8_t* const*)(vd + 0x10);   // palette index per voxel, 0 = empty
        float scale = it != g_scaled.end() ? it->second.origScale : *(const float*)(vd + 0x20);
        Vec3 pos = it != g_scaled.end() ? it->second.origPos : Vec3{ lt[0], lt[1], lt[2] };
        // (a shape sent far away by its mod is hidden)
        if (!v || !(scale > 0.001f && scale < 1.0f) || size[0] <= 0 || size[1] <= 0 || size[2] <= 0 ||
            !(Length(pos) < 10.0f))
            return;
        long long count = (long long)size[0] * size[1] * size[2];
        if (count > budget) return;
        budget -= count;
        Quat q = Normalize({ lt[3], lt[4], lt[5], lt[6] });
        Grid g{ pos, Rotate(q, { scale, 0, 0 }), Rotate(q, { 0, scale, 0 }), Rotate(q, { 0, 0, scale }),
                { size[0], size[1], size[2] }, v };
        g.dx = Dot(g.ax, f);
        g.dy = Dot(g.ay, f);
        g.dz = Dot(g.az, f);
        g.d0 = Dot(pos, f) + 0.5f * (g.dx + g.dy + g.dz);   // (voxel centres)
        g.vox = scale;
        grids.push_back(g);
    });
    // Every solid voxel with its depth along f (voxel index (z * sy + y) * sx + x).
    auto each = [&](auto fn) {
        for (const Grid& g : grids)
            for (int z = 0; z < g.n[2]; ++z)
                for (int y = 0; y < g.n[1]; ++y) {
                    const uint8_t* row = g.v + ((size_t)z * g.n[1] + y) * g.n[0];
                    float d = g.d0 + z * g.dz + y * g.dy;
                    for (int x = 0; x < g.n[0]; ++x)
                        if (row[x]) fn(g, x, y, z, d + x * g.dx);
                }
    };
    float front = -1e9f, back = 1e9f, vox = 0;
    each([&](const Grid& g, int, int, int, float d) {
        if (d > front) { front = d; vox = g.vox; }
        back = std::min(back, d);
    });
    if (voxels) *voxels = 0;
    if (front < -1e8f) return {};
    const float slab = std::min(0.2f, std::max(2.5f * vox, 0.03f * (front - back)));
    double sx = 0, sy = 0, sz = 0;
    int n = 0;
    each([&](const Grid& g, int x, int y, int z, float d) {
        if (d < front - slab) return;
        Vec3 p = g.o + g.ax * (x + 0.5f) + g.ay * (y + 0.5f) + g.az * (z + 0.5f);
        sx += p.x;
        sy += p.y;
        sz += p.z;
        ++n;
    });
    Vec3 c{ (float)(sx / n), (float)(sy / n), (float)(sz / n) };
    if (voxels) *voxels = n;
    return c + f * (front + 0.5f * vox - Dot(c, f));
}

// The factor ScaleModTool measured for a tool (a value of g_autoScale: the factor
// itself, or a record holding it).
inline float MeasuredScale(float v) { return v; }
template<class T> float MeasuredScale(const T& v) { return v.s; }

// Muzzle of each mod tool (its body, unscaled), measured with its size and again
// whenever ScaleModTool measures its size again (another look of the same tool).
struct ToolPivot { Vec3 m; float measured = 0; };
std::unordered_map<std::string, ToolPivot> g_toolPivots;

// Before ScaleModTool: the muzzle of this tool, or its body origin until it is measured.
void SelectToolPivot(const std::string& name)
{
    auto p = g_toolPivots.find(name);
    g_scaledPivot = p != g_toolPivots.end() ? p->second.m : Vec3{};
}

// After ScaleModTool: a tool whose size has just been measured gets its muzzle measured
// (from its unscaled shapes and desktop pose) and is scaled about it at once, so it is
// never drawn - nor reported to the Lua mod - scaled about another point with that size.
void ModToolPivot(BYTE* player, const std::string& name, const Pose& desk, float s)
{
    BYTE* body = *(BYTE**)(player + kPlayerToolBody);
    auto a = g_autoScale.find(name);
    if (!body || body != g_scaledBody || a == g_autoScale.end()) return;
    float measured = MeasuredScale(a->second);
    auto p = g_toolPivots.find(name);
    if (p != g_toolPivots.end() && p->second.measured == measured) return;
    int n = 0;
    Vec3 m = ToolMuzzle(body, desk, false, &n);
    g_toolPivots[name] = { m, measured };
    Log("Engine: mod tool '%s': muzzle %.3f %.3f %.3f in its body (front of the barrel, %d voxels) - scaled "
        "about it, so that its shots leave the drawn barrel", name.c_str(), m.x, m.y, m.z, n);
    g_scaledPivot = m;
    if (s != 1.0f) ApplyToolScale(body, s);
}

// Diagnostics (debugLua), once per tool 1 s after it is scaled: its muzzle, the front
// of the tool as drawn (the same point, unless a shape escaped the scaling or the mod
// moved its barrel) and where both are in the world (tool body pose of the last frame).
void MuzzleCheck(const std::string& name, BYTE* body, const Pose& flat, float s, float dt)
{
    static std::string timed, done;
    static float t = 0;
    if (name != timed) { timed = name; t = 0; }
    t += dt;
    if (t < 1.0f || done == name) return;
    done = name;
    int n = 0;
    Vec3 m = g_scaledPivot, d = ToolMuzzle(body, flat, true, &n);
    const float* b = (const float*)(body + 0x40);   // tool body: world position, then rotation
    Pose bt{ Normalize({ b[3], b[4], b[5], b[6] }), { b[0], b[1], b[2] } };
    Vec3 wm = Mul(bt, Pose{ {}, m }).p, wd = Mul(bt, Pose{ {}, d }).p;
    Log("Engine diag: mod tool '%s' at %.0f%%: muzzle %.3f %.3f %.3f, front of the drawn tool %.3f %.3f %.3f "
        "(%d voxels, gap %.1f mm); in the world %.3f %.3f %.3f / %.3f %.3f %.3f", name.c_str(), s * 100.0f, m.x, m.y,
        m.z, d.x, d.y, d.z, n, Length(d - m) * 1000.0f, wm.x, wm.y, wm.z, wd.x, wd.y, wd.z);
}

// GetShapeLocalTransform / GetShapeWorldTransform of a shape of the held, resized mod
// tool, for the scripts of mods: where the mod put it (unscaled; the shape is moved
// there for the call). What a mod writes with SetShapeLocalTransform is unscaled, so a
// mod that reads a part back to place others from it (Portal Gun, the Thermite
// Cannon's reload, the AC130 radio) no longer scales them twice, and a shot started
// from a part (Thermite Cannon: barrel part x offset) leaves the muzzle like one
// started from the body. Calls with a second argument (the Lua mod's: it measures the
// tool) get the shape as drawn.
constexpr uint32_t kEntityHandle = 0x0c;      // entity -> its script handle (int, < 0 for client entities)
using PFN_ArgInt = int(__fastcall*)(void* args, int index, int def);
PFN_ArgInt g_luaArgInt = nullptr;             // the engine's ArgInt (+631b80)
uint32_t g_offArgCount = 0;                   // binding arguments -> number of Lua arguments (8)
PFN_LuaFn Real_GetShapeLocal = nullptr;
PFN_LuaFn Real_GetShapeWorld = nullptr;

// A shape of the resized tool from its script handle. Scripts read many shapes, most of
// them not the tool's. With fastToolShapes the engine's own handle lookup gives the entity
// at once (a shape of the tool: type 2, parent the tool body, voxels - what ForToolShapes
// lists). Otherwise (or that lookup not recognised) the tool's shapes are listed once per
// frame (THE EVERYTHING REVOLVER: ~1000 map nodes allocated per frame), then found at once.
BYTE* ToolShapeOfHandle(int handle)
{
    if (Cfg().fastToolShapes && EntityLookupReady() && g_gameObj) {
        if (!handle || !g_scaledBody) return nullptr;
        BYTE* e = EntityOfHandle(*(BYTE**)(g_gameObj + g_offGameScene), handle);
        return e && *(const uint16_t*)(e + kEntityType) == 2 && *(BYTE**)(e + kEntityParent) == g_scaledBody &&
                       *(BYTE**)(e + kShapeVox)
                   ? e
                   : nullptr;
    }
    static std::unordered_map<int, BYTE*> shapes;
    static BYTE* body = nullptr;
    static uint64_t frame = 0;
    if (body != g_scaledBody || frame != g_frameCounter) {
        body = g_scaledBody;
        frame = g_frameCounter;
        shapes.clear();
        if (body) ForToolShapes(body, [&](BYTE* sh) { shapes[*(const int*)(sh + kEntityHandle)] = sh; });
    }
    auto it = handle ? shapes.find(handle) : shapes.end();
    return it != shapes.end() ? it->second : nullptr;
}

__int64 ShapeAsMade(PFN_LuaFn real, void* ctx, void* args, void* ret, void* a4)
{
    float* lt = nullptr;
    Vec3 made, drawn;
    if (args && g_luaArgInt && g_scaledBody && g_scaledS != 1.0f && g_vrActive &&
        *(const int*)((const BYTE*)args + g_offArgCount) == 1)
        if (BYTE* pl = LocalPlayer())
            if (*(BYTE**)(pl + kPlayerToolBody) == g_scaledBody)
                if (BYTE* sh = ToolShapeOfHandle(g_luaArgInt(args, 0, 0))) {
                    auto f = g_scaled.find(sh);
                    float* p = (float*)(sh + kShapeLocal);
                    // (only a shape we scaled, unchanged since)
                    if (f != g_scaled.end() && p[0] == f->second.wrotePos.x && p[1] == f->second.wrotePos.y &&
                        p[2] == f->second.wrotePos.z) {
                        lt = p;
                        made = f->second.origPos;
                        drawn = f->second.wrotePos;
                    }
                }
    if (!lt) return real(ctx, args, ret, a4);
    lt[0] = made.x;
    lt[1] = made.y;
    lt[2] = made.z;
    __int64 r = real(ctx, args, ret, a4);
    lt[0] = drawn.x;
    lt[1] = drawn.y;
    lt[2] = drawn.z;
    return r;
}

__int64 __fastcall Hook_GetShapeLocal(void* ctx, void* args, void* ret, void* a4)
{
    return ShapeAsMade(Real_GetShapeLocal, ctx, args, ret, a4);
}

__int64 __fastcall Hook_GetShapeWorld(void* ctx, void* args, void* ret, void* a4)
{
    return ShapeAsMade(Real_GetShapeWorld, ctx, args, ret, a4);
}

__int64 __fastcall Hook_ToolAnim(void* self, float dt, void* a3, void* a4)
{
    BYTE* player = *(BYTE**)((BYTE*)self + g_offToolPlayer);
    bool apply = g_vrActive && player && player[g_offToolFlag];
    Pose hand;
    EngineToolInfo info;
    bool scriptSet = false;
    Pose scriptT;
    if (apply) {
        hand = ReadPose(player + g_offToolOverride);
        // The right hand the mod placed on its tool this frame (the engine does not
        // overwrite it while the override flag is set).
        if (g_offToolHandR && player[g_offToolHandSet] && player[g_offToolHandRUsed]) {
            info.hasHand = true;
            info.hand = ReadPose(player + g_offToolHandR).p;
        }
        // This frame's SetToolTransform, consumed here: a mod that stops setting it
        // gets the default (identity), as the engine resets the field every frame.
        scriptSet = g_scriptToolTSet;
        scriptT = g_scriptToolT;
        g_scriptToolTSet = false;
    }
    __int64 r = Real_ToolAnim(self, dt, a3, a4);
    if (apply) {
        std::string name = CurrentToolName((BYTE*)self);
        if (player == LocalPlayer()) {   // (Input: the sledge on the trigger, see SledgeUse in input.cpp)
            g_sledgeHeld.store(name == "sledge");
            g_sledgeFrame.store(g_frameCounter);
        }
        const ToolRest& rest = name.empty() ? ToolRest{} : RestPoseOf(name);
        if (rest.known && g_offToolAnimPose) {
            // The tool's own animation (swing, recoil, sway) relative to its rest
            // pose, applied in the hand: tool = hand * grip^-1 * rest^-1 * animated.
            // At rest the tool is exactly in the hand, barrel along the hand's -Z.
            Pose anim = ReadPose((BYTE*)self + g_offToolAnimPose);
            Pose held{ {}, rest.grip * -1.0f };
            WritePose(player + g_offToolFinal, Mul(Mul(Mul(hand, held), Inverse(rest.rest)), anim));
            info.kind = 1;
            g_scriptToolName.clear();
            g_scaledBody = nullptr;
            g_dual.prof = nullptr;
        } else if (!name.empty() && !rest.known && Real_SetToolTransform && g_offPlayerList) {
            // A tool added by a mod: held relative to the hand exactly as the mod shows
            // it relative to the eye on a desktop screen, animations included. The Lua
            // mod has already moved the hand by the inverse of this tool's desktop
            // "hand" (which point of the tool the hand holds): tool = hand * flat.
            info.kind = 2;
            // (dual wield: a tool of two guns - see DualEye)
            g_dual.prof = nullptr;
            for (const DualProfile& p : kDualProfiles)
                if (name == p.tool) g_dual.prof = &p;
            g_dual.profFrame = g_frameCounter;
            info.flat = ScriptToolFlat(name, scriptSet ? scriptT : Pose{}, dt);
            const Pose desk = scriptSet ? scriptT : info.flat;   // (what the size and the muzzle are measured from)
            SelectToolPivot(name);
            info.scale = ScaleModTool(player, name, info.flat, scriptSet ? &scriptT : nullptr, dt);
            ModToolPivot(player, name, desk, info.scale);
            // The desktop picture shrunk by s toward the eye, animations included (a recoil
            // kick is as big as the tool), with the tool body placed so that its muzzle m
            // (the pivot of the scaling) is on the drawn barrel: the shapes are drawn at
            // m + s (p - m) for a point p of the mod's tool, and
            //   (q, s pos - (1 - s) q m) x (m + s (p - m)) = s (pos + q p).
            // The Lua mod gets this pose (it decides the grip from it) and m.
            Vec3 m = info.scale != 1.0f ? g_scaledPivot : Vec3{};
            info.flat.p = info.flat.p * info.scale - Rotate(info.flat.q, m) * (1.0f - info.scale);
            WritePose(player + g_offToolFinal, Mul(hand, info.flat));
            info.hand = m + (info.hand - m) * info.scale;   // (placed by the mod on the unscaled tool)
            info.pivot = m;
            if (Cfg().debugLua && info.scale != 1.0f && g_scaledBody)
                MuzzleCheck(name, g_scaledBody, desk, info.scale, dt);
        } else {
            // Not recognised: simply in the hand.
            WritePose(player + g_offToolFinal, hand);
            g_scriptToolName.clear();
            g_scaledBody = nullptr;
            g_dual.prof = nullptr;
        }
        g_toolInfo = info;
    } else if (player && player == LocalPlayer()) {
        g_toolInfo = {};
        g_dual.prof = nullptr;
    }
    return r;
}

// The map camera of the left eye (see MapView). view.q is already the head rotation.
void MapCamera(BYTE* camObj, int phase, float blend, Pose& view)
{
    float s = 0.5f - 0.5f * std::cos(3.14159265f * blend);   // the game's own easing
    if (phase == 1) {
        if (!g_map.on) {
            g_map.on = true;
            g_map.from = g_lastViewPos;
            g_map.eyeRef = g_leftEyePos;
            g_map.yaw = g_anchorYaw * 3.14159265f / 180.0f + YawOf(g_leftEyeRot);
            g_map.anchor = g_anchorYaw;
            Log("Engine: level map opened in 3D");
        }
        // Turning (right stick) rotates the room: the view then orbits around the
        // point looked at, so the map turns in front of the player.
        float turn = g_anchorYaw - g_map.anchor;
        if (turn > 180) turn -= 360;
        if (turn < -180) turn += 360;
        g_map.yaw += turn * 3.14159265f / 180.0f;
        g_map.anchor = g_anchorYaw;
        Vec3 smoothed = *(Vec3*)(camObj + g_offMapPos), target = *(Vec3*)(camObj + g_offMapTarget);
        float d = std::max(smoothed.y - target.y, 5.0f);
        float k = d / kMapTableDist;
        Vec3 center{ smoothed.x, target.y, smoothed.z };
        Vec3 back = Rotate(AxisAngle({ 0, 1, 0 }, g_map.yaw), { 0, 0, 1 });
        Vec3 head = Rotate(AxisAngle({ 0, 1, 0 }, g_anchorYaw * 3.14159265f / 180.0f), g_leftEyePos - g_map.eyeRef) * k;
        Vec3 m = center + back * (std::cos(kMapElevation) * d) + Vec3{ 0, std::sin(kMapElevation) * d, 0 } + head;
        g_map.last = m;
        g_map.lastK = k;
        view.p = g_map.from + (m - g_map.from) * s;
        g_curK = 1.0f + (k - 1.0f) * s;
    } else {
        // Closing: from the 3D map back down to the player's eyes.
        view.p = g_map.last + (view.p - g_map.last) * s;
        g_curK = g_map.lastK + (1.0f - g_map.lastK) * s;
    }
}

// Diagnostics (debugLua): where the held tool is drawn relative to the camera
// actually rendered, per eye; its change from one pair to the next is what the
// player sees as the tool trembling.
void ToolVsCamera(const Pose& camPose, int eye)
{
    if (!g_offPlayerList || !g_gameObj) return;
    BYTE** list = *(BYTE***)(g_gameObj + g_offPlayerList);
    int index = *(int*)(g_gameObj + g_offPlayerIndex);
    if (!list || index < 0 || index > 64 || !list[index]) return;
    const float* t = (const float*)(list[index] + 0x860);   // tool body world position (written at draw)
    Vec3 rel = Rotate(Conjugate(camPose.q), Vec3{ t[0], t[1], t[2] } - camPose.p);
    static Vec3 prev[2];
    static bool has[2];
    static float maxJ[2], sumJ[2];
    static int n[2];
    if (has[eye]) {
        float j = Length(rel - prev[eye]);
        maxJ[eye] = std::max(maxJ[eye], j);
        sumJ[eye] += j;
    }
    prev[eye] = rel;
    has[eye] = true;
    // is the "tool override" flag set when this eye is drawn?
    static int flagOff[2];
    if (g_offToolFlag && !list[index][g_offToolFlag]) ++flagOff[eye];
    // render interpolation factor of this frame (game+0x21c) and the fixed-step accumulator
    static float alphaA = 0, maxDa = 0;
    float alpha = *(float*)(g_gameObj + 0x21c);
    if (eye == 0) alphaA = alpha;
    else maxDa = std::max(maxDa, std::abs(alpha - alphaA));
    if (++n[eye] == 300) {
        Log("Engine: tool vs %s camera over 300 frames: max change %.1f mm, mean %.1f mm (now %.3f %.3f %.3f) alpha %.3f acc %.4f maxAlphaDiff %.3f, override flag off in %d frames",
            eye ? "right" : "left", maxJ[eye] * 1000, sumJ[eye] / 299 * 1000, rel.x, rel.y, rel.z, alpha,
            *(float*)(g_gameObj + g_offAcc1), maxDa, flagOff[eye]);
        flagOff[eye] = 0;
        if (eye) maxDa = 0;
        n[eye] = 0;
        maxJ[eye] = sumJ[eye] = 0;
    }
}

// Camera imposed by a mod. SetCameraTransform puts the camera controller
// (game+0x60) in mode 8 for that frame: its camera becomes the script's
// transform and SetCameraOffsetTransform (our eye offset) is ignored. The VR
// view then kept only that position, with the head's rotation in the room: from
// the AC130's plane you faced the horizon (the target far below), without head
// position. Now the mod's camera is the origin of a room: the direction the
// head faced when it started shows exactly the mod's view, and the head looks
// around and leans from there, with the normal eye separation. Its roll is
// removed (the horizon never tilts sideways). A mod camera at the player's own
// eyes (camera mods, scopes - reported by the Lua mod) keeps the normal head
// view: its rotation follows the player's aim, i.e. the hand.
struct ScriptViewState
{
    uint64_t lastFrame = 0;  // last engine frame shown from a mod camera
    float yaw0 = 0;          // head heading (headset space, radians) when it started
    Vec3 eyeRef;             // head position (headset space) when it started
} g_script;
int g_scriptMode = 0;          // camera mode set by SetCameraTransform (8), 0 = not recognised
bool g_camNearPlayer = false;  // Lua: the last frame's camera was at the player's eyes

// Zoom of a mod camera away from the player (modCameraZoom). A mod zooms by
// narrowing its field of view (the AC130: SetCameraTransform(t, zoomlevel * 10),
// 60 degrees at first, 5 at the closest, 5 degrees per mouse wheel notch), and
// Hook_CamSet replaces it with the headset's, so its zoom was lost. The mod's
// field of view is read from the projection the game has just built (1 / m5 =
// tan(fov / 2) * 9/16) and the headset image is magnified by
// tan(30 deg) / tan(fov / 2): 60 degrees (16:9 horizontal, about what a desktop
// monitor covers) or wider is shown at real size, narrower is magnified (AC130:
// up to x13.2). Both eyes' projections are scaled about their own optical axis
// (m0 and m5 times the zoom, the off-centre shifts m8 / m9 kept): infinity stays
// fused and the disparity grows with the zoom, like the world seen that much
// closer. The zoom eases in (mods zoom in steps) and is computed on the left
// eye's frame only: both eyes of a stereo pair use the same one.
constexpr float kZoomRefTanV = 0.57735027f * 0.5625f;   // tan(60 deg / 2) * 9/16
constexpr float kZoomMax = 16.0f;
struct ModZoomState
{
    float m = 1;      // eased zoom (0 = a new view: start at the mod's zoom)
    float pair = 1;   // zoom of the current stereo pair (1 = none)
} g_zoom;

float ModZoom(float modTanV)
{
    float target = 1;
    if (Cfg().modCameraZoom && modTanV > 1e-4f) target = std::clamp(kZoomRefTanV / modTanV, 1.0f, kZoomMax);
    static float logged = 1;
    if (std::abs(target - logged) > 0.01f * logged) {
        logged = target;
        Log("Engine: mod camera zoom x%.2f (mod field of view %.1f deg)", target,
            modTanV > 0 ? 2.0f * std::atan(modTanV / 0.5625f) / kDeg : 0.0f);
    }
    if (g_zoom.m < 1) {
        g_zoom.m = target;
    } else {
        float a = 1.0f - std::exp(-std::clamp(g_lastLeftDt, 0.0f, 0.1f) / 0.12f);
        g_zoom.m = std::exp(std::log(g_zoom.m) + (std::log(target) - std::log(g_zoom.m)) * a);
        if (std::abs(g_zoom.m - target) < 0.002f * target) g_zoom.m = target;
    }
    g_zoom.pair = g_zoom.m;
    return g_zoom.m;
}

void ApplyModZoom(float* proj, float zoom)
{
    if (zoom <= 1.0001f) return;
    proj[0] *= zoom;   // (m8 / m9 kept: magnified about the eye's optical axis)
    proj[5] *= zoom;
}

// Zoom by approach (modZoomMode 0, the default). The magnifier above cannot look right
// in a headset: it stretches a narrow rectilinear picture over the headset's ~100 degree
// field, so only its centre can stay still in the room when the head turns
// (PartialLook). A point seen a degrees off-centre slides with the head by
// sin^2(a) (1 - 1/M^2) of its rotation (50 degrees at x13: 58 %), the runtime's
// reprojection (it assumes a normal picture) disagrees by as much every frame, a zoom
// step grows the centre faster than the edges (sin 2a / 2a: 0.56 at 50 degrees) and the
// stereo depth is squashed M times (flat cut-outs): "a fisheye whose curvature
// changes". Instead the camera moves along the mod's axis toward the point it aims at,
// by (1 - 1/M) of its distance D: that point is seen M times bigger, with true
// perspective, while the projection, head rotation, head motion and eye separation stay
// exactly the headset's (the world stays rigid, the runtime's reprojection is exact).
// D comes from the Lua mod (TDVRZ: a ray along the mod's camera of the previous frame,
// sent every left-eye frame while a mod's camera is away from the player; small or fast
// objects such as shells and flying debris are skipped there). At x13 the camera is only
// D/13 from the point, so D must be followed closely: the AC130's plane circles, and the
// distance to a fixed target changes by up to ~15 m/s (a lag of 0.8 s made the target
// shrink to x9). It is followed in 0.1 s; a closer surface counts at once (the newest
// distances), a farther one is the farthest of the last few (something crossing the axis
// for a moment is closer than the surface behind it) and the camera moves forward toward
// it at most kDollyMaxSpeed of the distance per second (the aim left a roof: no rush).
// When something blocks the axis behind the camera (the aim swept onto a roof) the
// camera jumps straight to the new distance, like a cut, and it never comes closer than
// kDollyGap (x the relief factor) to the surface ahead. Until a distance is known in a
// view (only sky, or an old main.lua), or while the aimed surface is closer than 3 to 4
// kDollyGap (hysteresis; nothing to approach: a camera against a wall), the magnifier
// is used.
constexpr float kDollyGap = 2.0f;        // metres kept before the surface on the axis
constexpr float kDollyTau = 0.1f;        // s: time constant the aimed distance is followed with
constexpr float kDollyMaxSpeed = 0.2f;   // forward: at most this fraction of the distance per second
struct DollyZoomState
{
    float dist[8] = {};       // the last aimed distances (TDVRZ, one per left-eye frame), metres (0 = nothing hit)
    uint64_t frame[8] = {};   // engine frame each one arrived in
    unsigned n = 0;           // messages received (next slot: n % 8)
    uint64_t since = 0;       // only messages from this engine frame on count (a new view, a camera cut, a jump)
    float ref = 0;            // smoothed distance the approach is computed from (0 = none yet)
    Vec3 lastPos;             // the mod's camera at the previous left-eye frame (camera cuts)
    float uiDist = 0;         // aimed point - camera this pair (0 = the HUD stays at infinity / hudDistance)
    Vec3 aim;                 // the aimed point (world) this pair: the canvas centre is drawn on it
    bool loupe = true;        // the magnifier is used (hysteresis)
} g_dolly;

// Travel of the camera along the mod's axis (metres) for zoom M, in a left-eye frame
// (modPos / axis: the mod's camera and its aim direction, k: relief factor). While
// zoomed, also puts the canvas centre on the aimed point. < 0: use the magnifier (no
// aimed distance yet in this view, or nothing to approach).
float DollyTravel(float zoom, float k, const Vec3& modPos, const Vec3& axis)
{
    // A camera cut (the mod's camera jumped): distances measured before it no longer count.
    if (g_dolly.ref > 0 && Length(modPos - g_dolly.lastPos) > 30.0f) {
        g_dolly.ref = 0;
        g_dolly.since = g_frameCounter + 1;
    }
    g_dolly.lastPos = modPos;
    // The farthest distance of the last ~8 and ~3 left-eye frames (messages every 2 frames).
    float far8 = 0, far3 = 0;
    for (int i = 0; i < 8; ++i) {
        const uint64_t f = g_dolly.frame[i];
        if (!f || f < g_dolly.since || f > g_frameCounter) continue;
        const uint64_t age = g_frameCounter - f;
        if (age <= 16) far8 = std::max(far8, g_dolly.dist[i]);
        if (age <= 5) far3 = std::max(far3, g_dolly.dist[i]);
    }
    const float frac = 1.0f - 1.0f / std::max(zoom, 1.0f), gap = kDollyGap * k;
    const float dt = std::clamp(g_lastLeftDt, 0.0f, 0.1f);
    if (far8 > 0.1f) {
        if (g_dolly.ref <= 0) {
            g_dolly.ref = far8;
            Log("Engine: mod camera zoom by approach (aimed point %.0f m along its axis)", far8);
        } else if (far3 > 0.1f && far3 < frac * g_dolly.ref) {
            // Something now blocks the axis behind the camera (the aim swept onto a roof):
            // straight to the new distance, in one jump like a camera cut (a fast smooth move
            // backwards would be worse in a headset), not stuck against the surface. The
            // farther distances measured before no longer count (they would pull it forward
            // again for a moment).
            g_dolly.ref = far3;
            if (g_frameCounter > 5) g_dolly.since = std::max(g_dolly.since, g_frameCounter - 5);
        } else {
            const float target = far3 > 0.1f && far3 < g_dolly.ref ? far3 : far8;
            float step = (target - g_dolly.ref) * (1.0f - std::exp(-dt / kDollyTau));
            if (step > 0) step = std::min(step, kDollyMaxSpeed * g_dolly.ref * dt);
            g_dolly.ref += step;
        }
    }
    // (no distance yet, or the aimed surface too close to approach it: magnifier)
    g_dolly.loupe = g_dolly.ref < (g_dolly.loupe ? 4.0f : 3.0f) * gap;
    if (g_dolly.loupe) return -1;
    float travel = frac * g_dolly.ref;
    if (far3 > 0.1f) {
        travel = std::min(travel, std::max(0.0f, far3 - gap));
        if (zoom > 1.0001f) {   // (not zoomed: the HUD stays at infinity / hudDistance, as before)
            g_dolly.aim = modPos + axis * far3;   // (where the mod's shots go)
            g_dolly.uiDist = far3 - travel;
        }
    }
    if (Cfg().debugLua) {
        static int n = 0;
        if (++n >= 300) {
            n = 0;
            Log("Engine: zoom by approach x%.2f: aimed point %.1f m (smoothed %.1f m), camera moved %.1f m along the "
                "mod's axis, %.1f m from the aimed point", zoom, far3, g_dolly.ref, travel, g_dolly.ref - travel);
        }
    }
    return travel;
}

// A head rotation (from straight ahead, -z) with its direction turned only `s` of
// the way; its roll about the view axis is kept whole (a magnification does not
// scale roll): swing-twist split about -z.
Quat PartialLook(const Quat& look, float s)
{
    Vec3 f = Rotate(look, { 0, 0, -1 });
    Vec3 axis = Cross({ 0, 0, -1 }, f);
    float len = Length(axis);
    float angle = std::atan2(len, -f.z);
    if (len < 1e-6f) {
        if (f.z <= 0) return look;   // straight ahead
        axis = { 0, 1, 0 };          // straight behind: about the vertical
        len = 1;
    }
    axis = axis * (1.0f / len);
    Quat twist = Conjugate(AxisAngle(axis, angle)) * look;
    return Normalize(AxisAngle(axis, angle * s) * twist);
}

// Heading (radians) of a rotation, also when it looks straight down or up:
// from the forward vector, and near the vertical from the up vector as well
// (exact without roll; with roll, looking straight down, yaw and roll are the same).
float HeadingOf(const Quat& q)
{
    Vec3 f = Rotate(q, { 0, 0, -1 }), u = Rotate(q, { 0, 1, 0 });
    float s = std::clamp(f.y, -1.0f, 1.0f), c = std::sqrt(1.0f - s * s);
    Vec3 h = f * c - u * s;
    h.y = 0;
    h = h * (1.0f / std::max(Length(h), 1e-6f));
    if (c > 0.1f) {
        float t = std::clamp((c - 0.1f) / 0.2f, 0.0f, 1.0f);   // 1 within ~72 degrees of the horizon
        h = h * (1.0f - t) + Vec3{ f.x / c, 0, f.z / c } * t;
    }
    return std::atan2(-h.x, -h.z);
}

// The left eye's view from a mod camera (view = the mod's camera on entry).
void ScriptView(Pose& view, float modTanV)
{
    if (!g_script.lastFrame || g_frameCounter - g_script.lastFrame > 60) {
        g_zoom.m = 0;   // (a new view starts at the mod's current zoom, without a zoom animation)
        g_dolly.ref = 0;   // (and measures again how far its aimed point is: DollyTravel)
        g_dolly.since = g_frameCounter;
        g_script.yaw0 = HeadingOf(g_leftEyeRot);
        g_script.eyeRef = g_leftEyePos;
        Log("Engine: mod camera shown in VR (the head looks around from it)");
    }
    g_script.lastFrame = g_frameCounter;
    const Vec3 modPos = view.p, axis = Rotate(view.q, { 0, 0, -1 });   // the mod's camera and its aim axis
    float pitch = std::asin(std::clamp(axis.y, -1.0f, 1.0f));
    Quat base = AxisAngle({ 0, 1, 0 }, HeadingOf(view.q)) * AxisAngle({ 1, 0, 0 }, pitch);
    Quat room = base * AxisAngle({ 0, 1, 0 }, -g_script.yaw0);   // start direction of the head = the mod's
    float k = std::clamp(Cfg().modCameraDepth, 1.0f, 30.0f);      // > 1: the world looks like a model (more relief)
    float zoom = ModZoom(modTanV);
    // Zoom by approach (the default): the camera moves along the mod's axis (DollyTravel).
    float travel = Cfg().modZoomMode == 0 ? DollyTravel(zoom, k, modPos, axis) : -1.0f;
    if (travel < 0 && zoom > 1.0001f) {
        // Magnifier (modZoomMode 1, or no aimed distance yet): the head turns the camera
        // only 1/zoom as far, so the magnified world stays still in the room (at its
        // centre) instead of swimming zoom times faster than the head (and the head's
        // tremor is not magnified). The zoom is centred on the mod's axis, i.e. its
        // crosshair, which stays straight ahead of the start direction.
        view.q = base * PartialLook(AxisAngle({ 0, 1, 0 }, -g_script.yaw0) * g_leftEyeRot, 1.0f / zoom);
    } else {
        view.q = room * g_leftEyeRot;
    }
    view.p = view.p + Rotate(room, g_leftEyePos - g_script.eyeRef) * (Cfg().worldScale * k);
    if (travel >= 0) {
        view.p = view.p + axis * travel;
        g_zoom.pair = 1;   // (the headset's own projection: nothing is magnified)
    }
    g_curK = k;
}

// 2D interface per eye (the game's HUD, every mod's Ui* drawing). The game
// draws it with an orthographic "UI camera" at game+0x2c8 (projection pixels ->
// clip space, then canvas -> pixels at +0x40), rebuilt at the start of every
// frame by Game::frame and copied after this hook (same draw function) to the
// 2D renderer and to each script's draw(). With the off-centre projection the
// screen centre is not straight ahead and differs between the eyes (Quest 3:
// ~15 degrees outwards and ~13 degrees low, opposite sides for the two eyes): a
// centred crosshair was seen double. The canvas -> pixels matrix gets a per-eye
// shift so the canvas centre is seen in the direction g_uiDir by both eyes:
// straight ahead of the head, or a mod camera's axis (where its crosshair aims).
// UiWorldToPixel inverts the same matrix (world markers stay exact) and
// UiWidth/UiHeight (difference of the unprojected corners) do not change.
uint32_t g_offUiCam = 0;        // Game -> UI camera (0x2c8)
uint32_t g_offRenderer = 0;     // Game -> 2D renderer R (0x30), which every Ui* draw builds its MVP from
uint32_t g_offRendererProj = 0; // R -> its copy of the UI camera: projection (0x08), then the view (+0x40)
bool g_uiReady = false;
Vec3 g_uiDir{ 0, 0, -1 };       // world direction of the canvas centre, set by the left eye

// [Ecran] pauseWide (on by default). In a level the game lays its 2D interface out on a canvas 1920 wide and
// 1920 x H / W tall: the UI camera above, canvas -> pixels = W / 1920 both ways (unless the window is wider than
// 16:9: H / 1080). In VR the window is the headset's render size, nearly square (3024 x 3264: canvas 1920 x 2072).
// menu.lua draws inside UiSafeMargins (1920 x 1080 in the middle) over black, so the main menu looks 16:9; the pause
// menu (data/ui/hud.lua pauseMenu) uses the whole canvas (its bottom bar at UiHeight() - 80) over the 3D view of the
// whole window, so the virtual screen showed a square. A paused single-player level ticks no script (Game::update
// ends before the ticks: InitPauseWide), so our Lua asks for no eye and the frame goes to the virtual screen. Such a
// frame is now drawn for a 16:9 screen, anamorphically: at the end of its update (WideDecide - the UI camera is
// already rebuilt for this frame, the drawing comes after) canvas -> pixels becomes (W / 1920, H / 1080), so the
// canvas is 1920 x 1080 (UiWidth / UiHeight, UiSafeMargins and the mouse all go through this camera) stretched over
// the whole window; the 3D view gets the game's own field of view at 16:9 (Hook_CamSet: m0 = m5 x 9/16) instead of
// the eye's projection, stretched the same way. vr.cpp shows the whole back buffer on a 16:9 quad: the desktop
// game's proportions, no render target resized, the mouse mapping unchanged (a point of the quad is the same fraction
// of the window). The game rebuilds both matrices every frame: the first frame that is not paused is the VR view as
// before (and the TAA history restarts on either side of the switch: Hook_Render).
struct WideState
{
    bool ready = false;              // the game's level state and pause fields found (InitPauseWide)
    uint32_t offState = 0, offPaused = 0;
    uint64_t frame = 0;              // the engine frame drawn 16:9 (g_frameCounter), 0 = none
    bool was = false;                // the last frame A was
    bool resetHistory = false;       // a switch either way: the next render starts a fresh TAA history
    bool screenLive = false;         // vr.cpp, last Present: non-eye frames go to the headset's virtual screen
    unsigned switches = 0;           // (logged: the first 40)
};
WideState g_wide;
bool WideNow() { return g_wide.frame && g_wide.frame == g_frameCounter; }

// Mods' 2D crosshairs on the controller's aim. A mod tool draws its crosshair (portal
// gun logo, chaingun reticle...) at UiCenter/UiMiddle: the canvas centre, straight
// ahead of the HEAD in VR, while the tool shoots where the controller aims (the
// player's camera is turned toward the laser target). Each eye gets the offset
// (canvas units) that moves a point at the canvas centre onto the laser target: both
// eyes project the same world point, so the crosshair is seen at the target's own
// distance. Hook_UiRectVisible applies it to the small UI quads drawn near the
// canvas centre only; the rest of the HUD does not move.
Vec3 g_aimRel;                 // laser target - eye camera, sent by the Lua mod (TDVRT)
uint64_t g_aimRelFrame = 0;    // engine frame of that message (a left-eye frame)
Vec3 g_aimWorld;               // laser target of the current stereo pair (world)
uint64_t g_aimPairFrame = 0;   // left-eye frame of that pair
float g_aimUiOff[2] = {};      // this frame's offset for crosshairs, canvas units
uint64_t g_aimUiFrame = 0;     // engine frame g_aimUiOff was computed for
unsigned g_aimUiMoved = 0;     // diagnostics (debugLua): UI quads moved onto the aim

// (cx, cy: clip-space position this eye gives the canvas centre; w, h: screen pixels;
// V: canvas -> pixels)
void AimUiOffset(const Pose& eyeCam, const float* proj, int eye, float cx, float cy, float w, float h, const float* V)
{
    if (!Cfg().modCrosshairAim || Cfg().debugFovTest || V[0] <= 0 || V[5] <= 0) return;
    if (eye == 0) {
        // Left eye: only a target sent during this frame's update, and not while a mod
        // sets the camera (SetCameraTransform: the view is from the mod's camera, and
        // its crosshair is on that camera's axis) or the level map moves it.
        float blend;
        BYTE* camObj = g_gameObj ? *(BYTE**)(g_gameObj + g_offGamePlayer) : nullptr;
        int mode = camObj ? *(int*)camObj : 0;
        if (g_aimRelFrame != g_frameCounter || !camObj || MapPhase(camObj, blend) != 0 ||
            (g_scriptCamMode && mode == g_scriptCamMode) || (g_scriptMode && mode == g_scriptMode))
            return;
        g_aimWorld = eyeCam.p + g_aimRel;
        g_aimPairFrame = g_frameCounter;
    } else if (g_aimPairFrame + 1 != g_frameCounter) {
        return;   // right eye: only when its left eye had one
    }
    Vec3 d = Rotate(Conjugate(eyeCam.q), g_aimWorld - eyeCam.p);   // view space (-z forward)
    float ax, ay;
    if (d.z < -0.01f) {
        ax = std::clamp(proj[0] * (d.x / -d.z) - proj[8], -3.0f, 3.0f);
        ay = std::clamp(proj[5] * (d.y / -d.z) - proj[9], -3.0f, 3.0f);
    } else {
        // Aiming behind this eye: off the image on that side (the crosshair is not seen).
        float l = std::sqrt(d.x * d.x + d.y * d.y) + 1e-6f;
        ax = 3.0f * d.x / l;
        ay = 3.0f * d.y / l;
    }
    g_aimUiOff[0] = (ax - cx) * 0.5f * w / V[0];
    g_aimUiOff[1] = -(ay - cy) * 0.5f * h / V[5];
    g_aimUiFrame = g_frameCounter;
    if (Cfg().debugLua && eye == 0) {
        static uint64_t since = 0;
        if (g_frameCounter - since >= 600) {
            Log("Engine: mods' crosshairs: %u UI quads moved onto the aim since the last report (left eye offset "
                "%.0f, %.0f canvas units, target %.2f m away)", g_aimUiMoved, g_aimUiOff[0], g_aimUiOff[1],
                Length(g_aimRel));
            since = g_frameCounter;
            g_aimUiMoved = 0;
        }
    }
}

// Mods' interactive 2D windows (UiMakeInteractive: THE EVERYTHING REVOLVER's ammo menu,
// spawn menus, mod settings...) used with the controllers, either hand. On a desktop the
// window follows the mouse: UiGetMousePos, UiIsMouseInRect and the buttons read the
// cursor's window pixel through the frame's UI camera. In VR that camera differs per eye
// (ApplyUiShift): the same window pixel was a canvas point about 465 canvas units (25
// degrees) apart in the two eyes' frames - a dragged bullet was drawn twice, hovering
// changed from one eye to the other - and nothing moved the mouse, with no cursor in the
// headset. While a script shows such a window (this stereo pair or the one before):
//   - the canvas converges at modUiDistance (a plane facing the eyes, both hands in front
//     of it) instead of infinity / hudDistance;
//   - the window's world markers (its script's UiWorldToPixel) are put on that plane,
//     where the point is seen from between the eyes: the same canvas point in both eyes
//     (WorldPixelView); those on the script's own held tool (the revolver's ammo ring
//     around its cylinder) as on a desktop screen, centred in the view (ToolRing);
//   - the left eye's frame finds where the ray of the hand pointing at the canvas (the
//     one whose trigger was pulled last on it or, with offHandPointer, on any of our 2D
//     interfaces - Input::PointerHand -, else the dominant one; never the other hand only
//     because its ray is on the canvas) meets that plane, and
//     in each eye's frame every Ui* function reading the mouse gets this eye's pixel of
//     that canvas point (Hook_MousePixel): one cursor, the same canvas point in both eyes,
//     which vr.cpp draws over each eye's image (EngineModUiCursor). input.cpp makes the
//     pointing hand's trigger the left mouse button and its grip the right one
//     (EngineModUiHand).
// A hand on one of our own panels (settings, VR inventory) is not a pointer (vr.cpp).
// Not on the map, in a mod camera's own view or a zoomed one (the view is not the head's).
struct ModUiPointerState
{
    Pose aim[2];             // controller aim rays of this pair's Lua request (recentred room space)
    bool ray[2] = {};        // tracked, and not pointing at one of our own panels
    float trigger[2] = {};
    uint64_t rayFrame = 0;   // engine frame of that request
    bool trigDown[2] = {};
    BYTE* ui = nullptr;      // UI context of the script that last called UiMakeInteractive
    uint64_t shownFrame = 0; // engine frame of that call
    bool ready = false;      // mouse and markers hooked (InitModUiPointer)
    bool pair = false;       // a window is shown: decided by the left eye for its stereo pair
    uint64_t pairFrame = 0;  // that left-eye frame
    int hand = -1;           // hand pointing: 0 left, 1 right, -1 none (its trigger / grip click)
    int pref = -1;           // hand whose trigger was pulled last on a window (-1: the dominant one; offHandPointer: Input::PointerHand)
    bool onWin[2] = {};      // each hand's ray on the window (left eye of the pair; vr.cpp, OffLaserStop)
    float u = -1, v = -1;    // cursor, canvas units
    bool parked = false;     // a menu on its floating panel and no ray on it: the mouse is beside the window
    bool pressParked[2] = {};   // that hand's trigger was pulled while its ray was beside the panel (until released)
    float px = 0, py = 0;    // this frame's cursor, window pixels
    uint64_t mouseFrame = 0; // engine frame px / py are for
    float cursor[2][2] = {}; // per eye: the cursor on the eye's image (0..1)
    uint64_t cursorFrame[2] = {};
    unsigned reads = 0;      // diagnostics: mouse reads given the cursor
} g_modUi;

// The window is shown in this eye's frame (0 left, 1 right) of the current stereo pair.
bool ModUiNow(int eye) { return g_modUi.pair && g_frameCounter == g_modUi.pairFrame + eye; }

float ModUiDistance() { return std::clamp(Cfg().modUiDistance, 0.3f, 5.0f); }

// Mods' menus as floating panels (modUiPanel, on by default). Above, a window's canvas converges
// at modUiDistance but stays locked to the head: Natural Phenomena's WEATHER sidebar (370 x 995
// canvas units at the canvas' left edge) was seen 45 degrees to the left, cut by the right eye's
// image, and moved with every head turn. Now the window part of the window script's drawing is
// drawn, in each eye's own frame, on a quad fixed in the room (recentred headset space), with no
// extra pass: every Ui* draw builds its MVP on the CPU at call time from the 2D renderer's copy
// of the UI camera, R+8 (pixels -> clip) x R+0x48 (canvas -> pixels) x M, and the shaders keep w
// (full 4x4). During that scope R+8 holds Q[eye] x inverse(R+0x48), Q = canvas -> this eye's clip
// space through the panel (PanelBuildQ): MVP = Q x M, a perspective-correct quad on the panel.
// Layout, the mouse (MousePixel, unprojected through the flat UI camera) and UiWorldToPixel stay
// flat and exact; the rest of the UI (game HUD, other mods, the HUD parts of the same script)
// stays where it was. The scope: from the script's UiMakeInteractive until the UiPop that leaves
// its UiPush level, or the end of its draw() (Hook_ScriptDraw); the whole draw for a script that
// draws its window before that call (wholeDraw) and for 0.4 s after a window drawn at the top
// level closed (its slide-out animation).
//   - A window whose script draws markers on the held mod tool (THE EVERYTHING REVOLVER's ammo
//     ring: ToolRing) is a "tool" session: with modUiToolFixed (on by default, as the player asked)
//     on its own panel placed by PanelToolPlace - straight ahead at eye height (pitch 0, not where
//     the head looks), anchor = the canvas centre where ToolRing centres the ring, the size the
//     head-locked canvas had, never saved nor moved by a grip; it follows only the player's moves
//     (PanelPairUpdate), never a turn of the head. Without it: the head-locked converged canvas, the
//     ring at the centre of the view (before). The rules below are the menus' ("menu" sessions).
//   - Placement: room-locked, opened straight in front of the gaze (its heading and pitch,
//     pitch clamped), at modUiDistance, k = 0.8 mm per canvas unit x modUiPanelScale. The canvas
//     point at the panel's centre (the anchor) is the content box's centre for a window smaller
//     than the canvas (the NP sidebar), else the canvas centre; it follows the box for the first
//     0.4 s (slide-in animations), then is locked until the window closes.
//   - It follows the player only when they really turn or move away: the gaze's heading more
//     than 10 degrees beside both the content and the heading its place is relative to (the gaze
//     when it opened or was last brought back: a menu put to one side stays there while the player
//     looks ahead, at it or between) for 0.5 s (looking up at the sky or down is not turning
//     away), or the panel too near / too far after walking; never while a ray is on it, a trigger
//     drags in it or a hand moves it. It then glides back, keeping where the player put it
//     relative to the gaze (straight in front when it was never moved).
//   - A grip moves it (vr.cpp, GripTarget; not under our own panels, composited over the game
//     image): on its frame band (30 to 200 canvas units around the content), from the hand that is
//     not pointing at it, or anywhere on it when its script's input queries were read and never
//     asked for the right mouse button ("rmb" / "grab": PanelInputQuery) - otherwise a grip on the
//     content stays the window's right click. Its place is kept per window (content box
//     signature, 16 at most, modUiPanelPlaces).
//   - Its first stereo pair is drawn nowhere (its anchor is not known yet); the background blur
//     (UiBackgroundBlur) is left out on it (PanelNoBlur); another mod window shown at the same time
//     puts both back as before (one mouse for both) while it is shown.
//   - Clip-preserving culling (PanelClipVisible), scissor of clipped windows mapped onto the
//     panel (PanelScissor), markers of the window drawn on the panel (WorldPixelView::SetPanel),
//     and a runtime check that nothing else rewrote R+8 during the scope (PanelGuard).
enum { kPanelUnknown = 0, kPanelMenu = 1, kPanelTool = 2 };
struct ModPanelPlace
{
    float yaw = 0;     // degrees from the gaze heading the session opened with (+ = to the left)
    float pitch = 0;   // degrees above the horizon, seen from the eyes
    float dist = 1;    // metres from the eyes
};
struct ModPanelSaved { int key[4] = {}; ModPanelPlace place; };
struct ModPanelUi { BYTE* ui = nullptr; int key[4] = {}; float au = 0, av = 0; uint64_t frame = 0; };
constexpr float kPanelUnit = 0.0008f;   // metres per canvas unit at modUiPanelScale 1 (NP's sidebar: 0.30 x 0.80 m)
constexpr int kPanelHist = 16;
struct ModPanelState
{
    bool ready = false;          // InitModPanel succeeded
    // The session: one window at a time (the first one shown).
    bool active = false;
    BYTE* ui = nullptr;          // its script's UI context
    BYTE* uiPrev = nullptr;      // the window of the last session (its place is kept while it closes)
    int kind = kPanelUnknown;
    bool failed = false;         // R+8 was rewritten inside a scope: this window as before (head-locked)
    int failures = 0;            // ... sessions that failed (3: panels off until the game restarts)
    uint64_t start = 0, shown = 0;   // engine frames: first, and last UiMakeInteractive of a panel-eligible draw
    float age = 0;               // seconds (left-eye frames)
    // Its place: recentred headset space (metres), the anchor (au, av) at pose.p; +x right, +y up,
    // +z toward the eyes.
    Pose pose;
    float k = kPanelUnit;
    float cw = 1920, ch = 1080;  // canvas (UiWidth, UiHeight)
    float au = 960, av = 540;
    bool anchorLocked = false;
    int key[4] = {};             // content box signature (x0, y0, w, h rounded to 10), from the lock
    bool keyValid = false;
    float heading = 0;           // gaze heading (radians) the place's yaw is relative to
    ModPanelPlace place;
    bool placed = false;         // the player's place (saved): pitch absolute; else the gaze's pitch
    bool savePending = false;    // put down before its key was known: saved at the lock
    Vec3 placeEye;               // eyes when placed / brought back
    bool gliding = false;
    Pose glideTo;
    bool standby = false;        // pose / place are this pair's standby (no window, straight in front of the gaze)
    float awayT = 0;             // seconds the gaze has been away from it
    int held = -1;               // hand moving it (vr.cpp)
    bool moveNew = false;        // vr.cpp's new place while held (from the next stereo pair: both eyes alike)
    Pose moveTo;
    bool rayOn = false;          // a hand's ray on it (left eye of the last pair)
    // Content box, canvas units: the window's tested quads (UiRect, UiImage, UiText... - buttons
    // never reach the test), quads of 95 % of the canvas or more left out.
    float bb[4] = {};            // being collected (left eye of this pair)
    bool bbOk = false;
    float bbPair[4] = {};        // the last complete pair's
    bool bbPairOk = false;
    float hist[kPanelHist][4] = {};
    int histN = 0, histI = 0;
    float hit[4] = {};           // union of the last pairs' (pointer, grab, gaze)
    bool hitOk = false;
    int stable = 0;              // pairs the box has not moved
    int scopes = 0, quads = 0, preQuads = 0, mouse = 0;   // this left eye: scopes, quads in / before them, buttons
    int emptyPairs = 0;
    bool wholeDraw = false;      // the script draws its window before UiMakeInteractive
    bool depth0 = false;         // the last scope began at the top level (the whole draw is the window)
    int preLast = 1;             // quads the script drew before its scope in the last pair (left eye)
    // The scope (R+8 swapped).
    bool swapped = false;
    BYTE* scopeUi = nullptr;
    int scopeDepth = 0;          // UiPush depth at UiMakeInteractive (-1: the whole draw)
    float* rp = nullptr;         // R+8
    float saved[16] = {}, written[16] = {}, view[16] = {};
    int eye = 0;
    // Per eye: canvas -> clip through the panel, and what it was built from.
    float Q[2][16] = {};
    uint64_t qFrame[2] = {};
    Pose eyeRoom[2], eyeWorld[2];
    float qIn[2][10] = {};       // m0 m5 m8 m9 of the projection, V0 V5 V12 V13, window w h
    // The draw being made (Hook_ScriptDraw).
    bool drawMain = false;       // a scene script's draw with the frame's UI camera
    BYTE* drawUi = nullptr;
    // After the window closed: its slide-out.
    BYTE* lingerUi = nullptr;
    float lingerT = 0;
    // The window reads the right mouse button (Hook_InputQuery).
    bool rmbAware = false;
    bool rmbHooks = false;
    bool argSeen = false;        // a key name its script asked for was read (else its right-button reads are unknown)
    bool argLogged = false;
    BYTE* rmbUis[16] = {};
    int rmbN = 0;
    // A window shown before at this UI context in this game session (PanelUiFind): its anchor and saved
    // place are used at once, but only provisionally - after a level change another mod's window can
    // have the same address, and a window can change its layout - until its content box confirms it
    // (PanelCacheCheck).
    bool cached = false;
    int cacheKey[4] = {};
    bool cachePlaced = false;    // ... its saved place was applied from that key
    bool rmbCached = false;      // ... rmbAware came from it (not from this session's reads)
    ModPanelPlace openPlace;     // where it would have opened without it (straight in front of the gaze)
    Pose openPose;
    bool visible = false;        // drawn on the panel at least once (its first pair is not: PanelBegin)
    uint64_t firstBegin = 0;     // engine frame of its first scope (0: none yet)
    // Another mod's window shown while this menu is on its panel (PanelOnInteractive): both as before.
    BYTE* otherUi = nullptr;
    uint64_t otherFrame = 0;
    float otherT = 0;            // seconds it has been shown (shared: gone)
    bool shared = false;
    unsigned blurs = 0;          // background blurs left out on the panel (PanelNoBlur)
    // Markers on the held mod tool (WorldPixelView): the revolver's ring.
    BYTE* toolUi = nullptr;
    uint64_t toolFrame = 0;
    float toolK = 0;             // metres per canvas unit of the tool ring's panel (PanelToolPlace)
    // Saved places (modUiPanelPlaces) and the windows seen in this game session.
    std::vector<ModPanelSaved> places;
    std::string placesText;
    bool placesParsed = false;
    ModPanelUi uis[16];
    unsigned logs = 0, scissors = 0, wholeDraws = 0, refusals = 0;
    unsigned drawn = 0, quadsTotal = 0;   // this session: scopes drawn on the panel, quads tested in them
    unsigned hiddenDraws = 0;             // ... scopes drawn nowhere (its first pair: PanelBegin)
} g_panel;

// The revolver's ammo ring (a window drawing on the held mod tool: kPanelTool) on a panel fixed in the
// room (modUiToolFixed, on by default: PanelToolPlace); otherwise head-locked at the centre of the view.
bool PanelToolFixed() { return Cfg().modUiCenterTool && Cfg().modUiToolFixed; }

// Windows of this kind are drawn on a panel: menus with modUiPanel, the tool ring with PanelToolFixed.
bool PanelKindOn(int kind) { return kind == kPanelMenu ? Cfg().modUiPanel : kind == kPanelTool && PanelToolFixed(); }

bool PanelEnabled()
{
    return g_panel.ready && (Cfg().modUiPanel || PanelToolFixed()) && Cfg().modUiPointer && g_vrActive &&
           !Cfg().debugFovTest && g_panel.failures < 3;
}

// The session's window is shown on its panel (a menu floating in the room, or the tool ring fixed there).
bool PanelSession() { return g_panel.active && PanelKindOn(g_panel.kind) && !g_panel.failed && PanelEnabled(); }

// A mod's menu is shown as a floating panel (this session).
bool PanelMenuSession() { return PanelSession() && g_panel.kind == kPanelMenu; }

bool PanelLog() { return g_panel.logs++ < 200; }

// (logs, and the mock tests' aiming: mockctl.py pointpanel) a pose of the panel, recentred headset space
const char* PanelPoseText(const Pose& p)
{
    static char b[160];
    const Vec3 n = Rotate(p.q, { 0, 0, 1 });
    snprintf(b, sizeof(b), "centre %.3f %.3f %.3f normal %.3f %.3f %.3f", p.p.x, p.p.y, p.p.z, n.x, n.y, n.z);
    return b;
}

// Between the eyes, recentred headset space (the left eye of the pair moved by half the eye offset).
Vec3 PanelEyeMid()
{
    return g_leftEyePos + Rotate(g_leftEyeRot, g_eyeOffset.p * (0.5f / std::max(0.1f, Cfg().worldScale)));
}

// Upright (no roll), +z toward the eye point.
Quat PanelFacing(const Vec3& c, const Vec3& eye)
{
    Vec3 t = eye - c;
    const float l = Length(t);
    if (l < 1e-4f) return {};
    t = t * (1.0f / l);
    return AxisAngle({ 0, 1, 0 }, std::atan2(t.x, t.z)) * AxisAngle({ 1, 0, 0 }, -std::asin(std::clamp(t.y, -1.0f, 1.0f)));
}

// The panel `dist` metres from the eyes in the direction (heading, pitch), radians; facing the eyes.
Pose PanelPoseAt(float heading, float pitch, float dist, const Vec3& eye)
{
    const Vec3 dir{ -std::sin(heading) * std::cos(pitch), std::sin(pitch), -std::cos(heading) * std::cos(pitch) };
    const Vec3 c = eye + dir * dist;
    return { PanelFacing(c, eye), c };
}

float GazePitchDeg() { return std::asin(std::clamp(Rotate(g_leftEyeRot, { 0, 0, -1 }).y, -1.0f, 1.0f)) / kDeg; }

// --- Saved places: "x0 y0 w h yaw pitch dist;..." (most recent first) ---
void PanelParsePlaces()
{
    ModPanelState& P = g_panel;
    const std::string& s = Cfg().modUiPanelPlaces;
    if (P.placesParsed && s == P.placesText) return;
    P.placesParsed = true;
    P.placesText = s;
    P.places.clear();
    size_t i = 0;
    while (i < s.size() && P.places.size() < 16) {
        size_t e = s.find(';', i);
        if (e == std::string::npos) e = s.size();
        ModPanelSaved p;
        float y = 0, pi = 0, d = 0;
        if (sscanf(s.substr(i, e - i).c_str(), "%d %d %d %d %f %f %f", &p.key[0], &p.key[1], &p.key[2], &p.key[3], &y, &pi,
                   &d) == 7 &&
            std::isfinite(y) && std::isfinite(pi) && std::isfinite(d)) {
            p.place = { std::clamp(y, -180.0f, 180.0f), std::clamp(pi, -80.0f, 80.0f), std::clamp(d, 0.35f, 3.0f) };
            P.places.push_back(p);
        }
        i = e + 1;
    }
}

bool PanelKeyMatch(const int a[4], const int b[4])
{
    for (int i = 0; i < 4; ++i)
        if (std::abs(a[i] - b[i]) > 30) return false;
    return true;
}

const ModPanelSaved* PanelFindPlace(const int key[4])
{
    PanelParsePlaces();
    for (const ModPanelSaved& p : g_panel.places)
        if (PanelKeyMatch(p.key, key)) return &p;
    return nullptr;
}

// Remembers a window's place (most recent first, 16 at most) and saves it (ConfigSaveKeys: that key
// only, on a worker thread).
void PanelStorePlace(const int key[4], const ModPanelPlace& place)
{
    ModPanelState& P = g_panel;
    PanelParsePlaces();
    for (size_t i = 0; i < P.places.size(); ++i)
        if (PanelKeyMatch(P.places[i].key, key)) {
            P.places.erase(P.places.begin() + i);
            break;
        }
    ModPanelSaved n;
    memcpy(n.key, key, sizeof(n.key));
    n.place = place;
    P.places.insert(P.places.begin(), n);
    if (P.places.size() > 16) P.places.resize(16);
    std::string s;
    for (const ModPanelSaved& p : P.places) {
        char b[96];
        snprintf(b, sizeof(b), "%s%d %d %d %d %.0f %.0f %.2f", s.empty() ? "" : ";", p.key[0], p.key[1], p.key[2], p.key[3],
                 p.place.yaw, p.place.pitch, p.place.dist);
        s += b;
    }
    Cfg().modUiPanelPlaces = s;
    P.placesText = s;
    ConfigSaveKeys({ "modUiPanelPlaces" });
}

ModPanelUi* PanelUiFind(BYTE* ui)
{
    for (ModPanelUi& u : g_panel.uis)
        if (u.ui && u.ui == ui) return &u;
    return nullptr;
}

void PanelUiRemember(BYTE* ui)
{
    ModPanelState& P = g_panel;
    ModPanelUi* slot = PanelUiFind(ui);
    if (!slot) {
        slot = &P.uis[0];
        for (ModPanelUi& u : P.uis) {   // (a free slot, else the oldest one)
            if (!u.ui) {
                slot = &u;
                break;
            }
            if (u.frame < slot->frame) slot = &u;
        }
    }
    slot->ui = ui;
    memcpy(slot->key, P.key, sizeof(slot->key));
    slot->au = P.au;
    slot->av = P.av;
    slot->frame = g_frameCounter;
}

bool PanelRmbKnown(BYTE* ui)
{
    for (int i = 0; i < g_panel.rmbN; ++i)
        if (g_panel.rmbUis[i] == ui) return true;
    return false;
}

// Anchor for a content box: its centre, on each axis where the window is clearly smaller than
// the canvas; the canvas centre otherwise (a full-screen menu keeps its layout around the gaze).
void PanelAnchorFrom(const float b[4], float& au, float& av)
{
    const ModPanelState& P = g_panel;
    au = b[2] - b[0] < 0.6f * P.cw ? 0.5f * (b[0] + b[2]) : 0.5f * P.cw;
    av = b[3] - b[1] < 0.75f * P.ch ? 0.5f * (b[1] + b[3]) : 0.5f * P.ch;
}

// Canvas -> this eye's clip space through the panel, from the inputs PanelBuildQ kept. In the
// eye's view space (-z forward) a canvas point is p = A u + B v + C; clip x = m0 x + m8 z,
// y = m5 y + m9 z, z = 0 (like the UI's own orthographic camera), w = -z. Column-major.
void PanelComputeQ(int eye)
{
    ModPanelState& P = g_panel;
    const Pose& e = P.eyeRoom[eye];
    const Quat ie = Conjugate(e.q);
    const Vec3 A = Rotate(ie, Rotate(P.pose.q, { P.k, 0, 0 }));
    const Vec3 B = Rotate(ie, Rotate(P.pose.q, { 0, -P.k, 0 }));
    const Vec3 C = Rotate(ie, P.pose.p - e.p) - A * P.au - B * P.av;
    const float* in = P.qIn[eye];
    float* Q = P.Q[eye];
    auto col = [&](float* c, const Vec3& a) {
        c[0] = in[0] * a.x + in[2] * a.z;
        c[1] = in[1] * a.y + in[3] * a.z;
        c[2] = 0;
        c[3] = -a.z;
    };
    col(Q, A);
    col(Q + 4, B);
    Q[8] = Q[9] = Q[10] = Q[11] = 0;
    col(Q + 12, C);
}

// Each eye, with the VR room camera (ApplyUiShift): Q for this frame. eyeCam: the eye's camera in
// the world; proj: its projection; w x h: window pixels; V: canvas -> pixels, already shifted.
void PanelBuildQ(int eye, const Pose& eyeCam, const float* proj, float w, float h, const float* V)
{
    ModPanelState& P = g_panel;
    if (!PanelEnabled() || V[0] <= 0 || V[5] <= 0) return;
    const Pose left{ g_leftEyeRot, g_leftEyePos };
    P.eyeRoom[eye] = eye == 0 ? left
                              : Mul(left, Pose{ g_eyeOffset.q, g_eyeOffset.p * (1.0f / std::max(0.1f, Cfg().worldScale)) });
    P.eyeWorld[eye] = eyeCam;
    float* in = P.qIn[eye];
    in[0] = proj[0];
    in[1] = proj[5];
    in[2] = proj[8];
    in[3] = proj[9];
    in[4] = V[0];
    in[5] = V[5];
    in[6] = V[12];
    in[7] = V[13];
    in[8] = w;
    in[9] = h;
    PanelComputeQ(eye);
    P.qFrame[eye] = g_frameCounter;
}

// A canvas point in this eye's image (0..1, y down). False behind the eye or without Q this frame.
bool PanelProject(int eye, float u, float v, float& x, float& y)
{
    const ModPanelState& P = g_panel;
    if (eye < 0 || eye > 1 || P.qFrame[eye] != g_frameCounter) return false;
    const float* Q = P.Q[eye];
    const float cx = Q[0] * u + Q[4] * v + Q[12], cy = Q[1] * u + Q[5] * v + Q[13], cw = Q[3] * u + Q[7] * v + Q[15];
    if (!(cw > 1e-4f)) return false;
    x = (cx / cw + 1.0f) * 0.5f;
    y = (1.0f - cy / cw) * 0.5f;
    return std::isfinite(x) && std::isfinite(y);
}

// A ray (recentred headset space) on the panel's plane, from its front: the canvas point and the
// distance along the ray (metres).
bool PanelRay(const Pose& aim, float& u, float& v, float& t)
{
    const ModPanelState& P = g_panel;
    const Vec3 n = Rotate(P.pose.q, { 0, 0, 1 }), d = Rotate(aim.q, { 0, 0, -1 });
    const float den = Dot(d, n);
    if (den > -0.05f) return false;
    t = Dot(P.pose.p - aim.p, n) / den;
    if (!(t > 0)) return false;
    const Vec3 l = Rotate(Conjugate(P.pose.q), aim.p + d * t - P.pose.p);
    u = P.au + l.x / P.k;
    v = P.av - l.y / P.k;
    return std::isfinite(u) && std::isfinite(v);
}

// Within the content box + margin (the whole canvas while no box is known).
bool PanelInBox(float u, float v, float margin)
{
    const ModPanelState& P = g_panel;
    if (!P.hitOk) return u >= 0 && u <= P.cw && v >= 0 && v <= P.ch;
    return u >= P.hit[0] - margin && u <= P.hit[2] + margin && v >= P.hit[1] - margin && v <= P.hit[3] + margin;
}

// Degrees the gaze's heading (from `eye`) is beside the span from the heading the panel's place is
// relative to (P.heading: the gaze when it opened, or when it was last brought back) to the content's
// horizontal extent (0 within it). A menu the player put to one side stays there while they look
// ahead, at it, or anywhere between; only looking beyond both is turning away (measured from the
// raw gaze alone, a menu put 45 degrees aside was "away" whenever the player looked ahead, and
// followed every head turn). Only the heading: looking up at the sky or down at the floor is not
// turning away (the panel would otherwise come and hide what the player looks at).
float PanelGazeOutside(const Vec3& eye, float heading)
{
    const ModPanelState& P = g_panel;
    const float x0 = P.hitOk ? P.hit[0] : 0, x1 = P.hitOk ? P.hit[2] : P.cw;
    auto headingOf = [&](float u) {
        const Vec3 d = P.pose.p + Rotate(P.pose.q, { (u - P.au) * P.k, 0, 0 }) - eye;
        return std::atan2(-d.x, -d.z);
    };
    auto wrap = [](float a) {
        while (a > kPi) a -= 2 * kPi;
        while (a < -kPi) a += 2 * kPi;
        return a;
    };
    // (headings as angles to the right of the content's left edge: the content is 0 .. width; the
    // span also covers the place's own heading)
    const float left = headingOf(x0), width = wrap(left - headingOf(x1));
    if (width <= 0) return 180.0f;   // (seen from behind)
    const float ref = wrap(left - P.heading);
    const float lo = std::min(0.0f, ref), hi = std::max(width, ref);
    const float into = wrap(left - heading);
    if (into >= lo && into <= hi) return 0;
    const float right = into > hi ? into - hi : into + 2 * kPi - hi;     // (beyond its right end)
    const float leftOf = into < lo ? lo - into : lo + 2 * kPi - into;    // (beyond its left end)
    return std::min(right, leftOf) / kDeg;
}

// The key of the content box of the last complete pair (the whole canvas while none is known).
void PanelKeyNow(int key[4])
{
    const ModPanelState& P = g_panel;
    const float full[4] = { 0, 0, P.cw, P.ch };
    const float* b = P.bbPairOk ? P.bbPair : full;
    auto r10 = [](float v) { return (int)std::lround(v / 10.0f) * 10; };
    key[0] = r10(b[0]);
    key[1] = r10(b[1]);
    key[2] = r10(b[2] - b[0]);
    key[3] = r10(b[3] - b[1]);
}

// The anchor is locked: the window's key, its saved place (it glides there), what to remember.
void PanelLockAnchor(const Vec3& eye)
{
    ModPanelState& P = g_panel;
    P.anchorLocked = true;
    const float* b = P.bbPair;
    const float full[4] = { 0, 0, P.cw, P.ch };
    if (!P.bbPairOk) b = full;
    PanelKeyNow(P.key);
    P.keyValid = true;
    P.cached = false;   // (a provisional anchor / place from an earlier opening: confirmed - PanelCacheCheck)
    PanelUiRemember(P.ui);
    if (P.savePending) {
        P.savePending = false;
        PanelStorePlace(P.key, P.place);
    } else if (const ModPanelSaved* s = P.placed ? nullptr : PanelFindPlace(P.key)) {
        P.place = s->place;
        P.placed = true;
        P.glideTo = PanelPoseAt(P.heading + P.place.yaw * kDeg, P.place.pitch * kDeg, P.place.dist, eye);
        P.gliding = P.held < 0;
    }
    if (PanelLog())
        Log("Engine: mod panel anchored (ui %p, anchor %.0f %.0f, content %s %.0f %.0f %.0f %.0f, key %d %d %d %d; k %.5f; %s)%s",
            P.ui, P.au, P.av, P.bbPairOk ? "box" : "unknown (whole canvas)", b[0], b[1], b[2], b[3], P.key[0], P.key[1],
            P.key[2], P.key[3], P.k, PanelPoseText(P.gliding ? P.glideTo : P.pose), P.gliding ? " - goes to its saved place" : "");
}

// The left eye's draw of the previous pair is complete: its content box and scopes.
void PanelPairStats()
{
    ModPanelState& P = g_panel;
    if (P.scopes > 0) {
        if (P.bbOk) {
            float d = 0;
            for (int i = 0; i < 4; ++i) d = std::max(d, std::abs(P.bb[i] - P.bbPair[i]));
            P.stable = P.bbPairOk && d < 3.0f ? P.stable + 1 : 0;
            memcpy(P.bbPair, P.bb, sizeof(P.bb));
            P.bbPairOk = true;
            memcpy(P.hist[P.histI], P.bb, sizeof(P.bb));
            P.histI = (P.histI + 1) % kPanelHist;
            P.histN = std::min(P.histN + 1, kPanelHist);
            P.hit[0] = P.hit[1] = 1e9f;
            P.hit[2] = P.hit[3] = -1e9f;
            for (int i = 0; i < P.histN; ++i) {
                P.hit[0] = std::min(P.hit[0], P.hist[i][0]);
                P.hit[1] = std::min(P.hit[1], P.hist[i][1]);
                P.hit[2] = std::max(P.hit[2], P.hist[i][2]);
                P.hit[3] = std::max(P.hit[3], P.hist[i][3]);
            }
            P.hitOk = true;
        }
        // (a scope that drew nothing at all - no tested quad, no button reading the mouse - while
        // the script drew before it: the window comes before UiMakeInteractive)
        if (P.quads == 0 && P.mouse == 0 && P.preQuads > 0 && !P.wholeDraw) {
            if (++P.emptyPairs >= 3) {
                P.wholeDraw = true;
                if (P.wholeDraws++ < 20)
                    Log("Engine: mod panel scope empty - whole draw (the script draws its window before UiMakeInteractive: "
                        "its HUD parts go to the panel too)");
            }
        } else if (P.quads > 0 || P.mouse > 0) {
            P.emptyPairs = 0;
        }
        P.preLast = P.preQuads;
    }
    P.bbOk = false;
    P.scopes = P.quads = P.preQuads = P.mouse = 0;
}

void PanelSessionEnd()
{
    ModPanelState& P = g_panel;
    P.active = false;
    if (P.kind == kPanelMenu && Cfg().modUiPanel && !P.failed && (P.depth0 || P.wholeDraw)) {
        P.lingerUi = P.ui;   // (its slide-out animation stays on the panel)
        P.lingerT = 0.4f;
    }
    if (P.kind == kPanelMenu && P.anchorLocked) PanelUiRemember(P.ui);
    if (PanelLog())
        Log("Engine: mod panel closed (ui %p, %s, %.1f s; %u eye draws on the panel, %u of them hidden while its anchor was "
            "unknown, %u quads tested in the left eye's%s)",
            P.ui,
            P.kind == kPanelMenu ? (P.shared ? "menu, shown as before with another window" : P.failed ? "menu, failed" : "menu")
            : P.kind == kPanelTool ? "tool ring"
                                   : "undecided",
            P.age, P.drawn, P.hiddenDraws, P.quadsTotal, P.wholeDraw ? "; whole draw" : "");
}

// The anchor / place of an earlier opening at this UI context (PanelSessionStart) are not this
// window's: forgotten (and its right-button mark); the anchor follows the content box again and,
// when that place was applied, the window goes back where it would have opened (at once while it
// was never drawn: its first pair is not - PanelBegin).
void PanelCacheDrop(const char* why)
{
    ModPanelState& P = g_panel;
    if (!P.cached) return;
    P.cached = false;
    if (ModPanelUi* u = PanelUiFind(P.ui)) u->ui = nullptr;
    if (P.rmbCached) {
        P.rmbCached = false;
        P.rmbAware = false;   // (read again from this window's own queries: PanelInputQuery)
        for (int i = 0; i < P.rmbN; ++i)
            if (P.rmbUis[i] == P.ui) {
                P.rmbUis[i] = P.rmbUis[--P.rmbN];
                break;
            }
    }
    const bool back = P.cachePlaced && P.held < 0;
    if (back) {
        P.cachePlaced = false;
        P.place = P.openPlace;
        P.placed = false;
        if (P.visible) {
            P.glideTo = P.openPose;
            P.gliding = true;
        } else {
            P.pose = P.openPose;
            P.gliding = false;
        }
    }
    if (P.bbPairOk) PanelAnchorFrom(P.bbPair, P.au, P.av);
    else {
        P.au = 0.5f * P.cw;
        P.av = 0.5f * P.ch;
    }
    if (PanelLog())
        Log("Engine: mod panel: not the window shown before at this UI context (key %d %d %d %d: %s) - its anchor is found "
            "again%s", P.cacheKey[0], P.cacheKey[1], P.cacheKey[2], P.cacheKey[3], why,
            back ? ", and it goes back in front of the gaze" : "");
}

// Left eye, while the anchor is not locked: is the provisional anchor / place of an earlier opening
// this window's? Its first complete pair: the content box's size (the box may still slide in: NP's
// x goes from -240 to 30 in 0.3 s); at the lock: the whole key.
void PanelCacheCheck(bool lock)
{
    ModPanelState& P = g_panel;
    if (!P.cached) return;
    int key[4];
    PanelKeyNow(key);
    if (lock) {
        if (!PanelKeyMatch(key, P.cacheKey)) PanelCacheDrop("its content box is elsewhere");
    } else if (P.bbPairOk && (std::abs(key[2] - P.cacheKey[2]) > 30 || std::abs(key[3] - P.cacheKey[3]) > 30)) {
        PanelCacheDrop("its content box has another size");
    }
}

// A window shows up (its script's UiMakeInteractive in a draw that can be a panel): its session,
// at this pair's standby place (straight in front of the gaze), or where it was put before.
void PanelSessionStart(BYTE* ui)
{
    ModPanelState& P = g_panel;
    PanelParsePlaces();
    P.active = true;
    P.ui = ui;
    P.kind = kPanelUnknown;
    P.failed = false;
    P.start = P.shown = g_frameCounter;
    P.age = 0;
    P.anchorLocked = P.keyValid = P.savePending = P.gliding = P.moveNew = false;
    P.awayT = 0;
    P.held = -1;
    P.bbOk = P.bbPairOk = P.hitOk = false;
    P.histN = P.histI = P.stable = 0;
    P.scopes = P.quads = P.preQuads = P.mouse = P.emptyPairs = 0;
    P.wholeDraw = P.depth0 = false;
    P.preLast = 1;
    P.lingerUi = nullptr;
    P.lingerT = 0;
    P.drawn = P.quadsTotal = P.hiddenDraws = 0;
    P.rmbAware = P.rmbCached = PanelRmbKnown(ui);
    P.argSeen = false;
    P.visible = false;
    P.firstBegin = 0;
    P.otherUi = nullptr;
    P.otherFrame = 0;
    P.otherT = 0;
    P.shared = false;
    P.cached = P.cachePlaced = false;
    if (!P.standby && P.uiPrev != ui) {
        // (another window was still closing or held: this one opens in front of the gaze)
        P.heading = HeadingOf(g_leftEyeRot);
        P.place = { 0, std::clamp(GazePitchDeg(), -50.0f, 25.0f), std::clamp(ModUiDistance(), 0.35f, 3.0f) };
        P.placed = false;
        P.pose = PanelPoseAt(P.heading, P.place.pitch * kDeg, P.place.dist, PanelEyeMid());
        P.au = 0.5f * P.cw;
        P.av = 0.5f * P.ch;
    }
    P.standby = false;
    P.uiPrev = ui;
    P.openPlace = P.place;
    P.openPose = P.pose;
    if (const ModPanelUi* u = PanelUiFind(ui)) {
        // (shown before at this UI context in this game session: its anchor at once, and its saved
        // place - provisional until its content box confirms it is that window, PanelCacheCheck)
        P.au = u->au;
        P.av = u->av;
        memcpy(P.cacheKey, u->key, sizeof(P.cacheKey));
        P.cached = true;
        if (const ModPanelSaved* s = PanelFindPlace(P.cacheKey)) {
            P.place = s->place;
            P.placed = true;
            P.cachePlaced = true;
            P.pose = PanelPoseAt(P.heading + P.place.yaw * kDeg, P.place.pitch * kDeg, P.place.dist, PanelEyeMid());
        }
    }
    for (int e = 0; e < 2; ++e)   // (this frame's Q: drawn there from this very frame)
        if (P.qFrame[e] == g_frameCounter) PanelComputeQ(e);
}

// The tool ring's window (kPanelTool with PanelToolFixed: THE EVERYTHING REVOLVER's ammo menu), as the
// player asked: it opens straight ahead at eye height - the gaze's heading, pitch 0, also when the head
// looks down at the gun - at modUiDistance, upright and facing the eyes, and stays there in the room: a turn
// of the head does not move it; the room carries it along with the stick's moves and turns, and it follows
// the player's own steps (PanelPairUpdate). Its anchor is the canvas centre, where ToolRing centres the ring
// (WorldPixelView::SetDesktop), and k = 2 D / (m0 cw) shows a canvas unit as large as the head-locked canvas
// did (m0: the left eye's projection): the ring keeps the size it had at the centre of the view, the window's
// side drawers their angle from it. Never saved, never moved by a grip (EngineGetModPanel: not movable - a
// grip on it stays the window's right click). Also when a menu turns out to draw on the held tool
// (WorldPixelView::Set), and when the setting is switched on while it is open (PanelPairUpdate).
void PanelToolPlace()
{
    ModPanelState& P = g_panel;
    const Vec3 eye = PanelEyeMid();
    const float dist = std::clamp(ModUiDistance(), 0.35f, 3.0f);
    const float m0 = P.qIn[0][0];
    P.toolK = m0 > 0 && P.cw > 0 ? std::clamp(2.0f * dist / (m0 * P.cw), 0.0003f, 0.005f) : kPanelUnit;
    P.k = P.toolK;
    P.heading = HeadingOf(g_leftEyeRot);
    P.place = { 0, 0, dist };
    P.placed = false;
    P.pose = PanelPoseAt(P.heading, 0, dist, eye);
    P.placeEye = eye;
    P.gliding = P.moveNew = false;
    P.au = 0.5f * P.cw;
    P.av = 0.5f * P.ch;
    P.anchorLocked = true;
    P.keyValid = P.savePending = false;
    P.cached = P.cachePlaced = false;
    for (int e = 0; e < 2; ++e)   // (this frame's Q: on its new place from this very frame)
        if (P.qFrame[e] == g_frameCounter) PanelComputeQ(e);
}

void PanelEnd(const char* why);   // (below: the floating panel's scope ends)

// Left eye of a stereo pair, with the VR room camera (ApplyUiShift, before its canvas shift): the
// previous pair's statistics, the session's end, the standby place, a move from vr.cpp, the
// anchor, and bringing the panel back in front when the player turned or moved away.
void PanelPairUpdate(float cw, float ch)
{
    ModPanelState& P = g_panel;
    if (!PanelEnabled()) {
        if (P.active) PanelSessionEnd();
        return;
    }
    const float dt = std::clamp(g_lastLeftDt, 0.0f, 0.1f);
    P.cw = cw;
    P.ch = ch;
    // (the tool ring's panel keeps its own scale: PanelToolPlace)
    P.k = P.active && P.kind == kPanelTool && PanelToolFixed() && P.toolK > 0
              ? P.toolK
              : kPanelUnit * std::clamp(Cfg().modUiPanelScale, 0.5f, 2.0f);
    PanelPairStats();
    if (P.lingerT > 0 && (P.lingerT -= dt) <= 0) P.lingerUi = nullptr;
    if (P.active && g_frameCounter - P.shown > 2) PanelSessionEnd();
    const Vec3 eye = PanelEyeMid();
    const float heading = HeadingOf(g_leftEyeRot);
    if (P.moveNew) {   // (also just after the window closed: the place it is put down at)
        P.pose = P.moveTo;
        P.moveNew = false;
        P.gliding = false;
    }
    if (!P.active) {
        if (P.lingerUi || P.held >= 0) return;   // (the closing window keeps its place)
        // Standby: where a window opening in this pair appears - straight in front of the gaze.
        P.heading = heading;
        P.place = { 0, std::clamp(GazePitchDeg(), -50.0f, 25.0f), std::clamp(ModUiDistance(), 0.35f, 3.0f) };
        P.placed = false;
        P.pose = PanelPoseAt(heading, P.place.pitch * kDeg, P.place.dist, eye);
        P.placeEye = eye;
        P.au = 0.5f * cw;
        P.av = 0.5f * ch;
        P.standby = true;
        return;
    }
    P.age += dt;
    if (!PanelKindOn(P.kind) || (P.failed && !P.shared)) return;
    // (another mod's window shown at the same time for 0.5 s - not a menu fading out while this one
    // opens: both as before, one canvas converged in front of the head and one mouse at the same canvas
    // point for both; on the panel, the other window lost its convergence and got the panel's cursor,
    // unseen. Back on the panel 1 s after it is gone.)
    const bool other = P.otherFrame && g_frameCounter - P.otherFrame <= 2;
    if (P.shared) {
        P.otherT = other ? 0.0f : P.otherT + dt;
        if (P.otherT < 1.0f) return;
        P.failed = P.shared = false;
        P.otherT = 0;
        P.awayT = 0;
        if (PanelLog()) Log("Engine: mod panel: the other mod window is gone - this one is back on its floating panel");
    } else if (other) {
        if ((P.otherT += dt) >= 0.5f) {
            P.failed = P.shared = true;
            P.otherT = 0;
            if (P.swapped) PanelEnd(nullptr);
            if (PanelLog())
                Log("Engine: mod panel: another mod window is shown at the same time (ui %p) - both are shown as before, in "
                    "front of the head, while it is", P.otherUi);
            return;
        }
    } else {
        P.otherT = 0;
    }
    if (P.kind == kPanelTool) {
        if (!P.anchorLocked) PanelToolPlace();   // (its setting was switched on while it was open)
    } else if (!P.anchorLocked) {
        PanelCacheCheck(false);
        if (P.bbPairOk && !P.cached) PanelAnchorFrom(P.bbPair, P.au, P.av);
        if (P.age >= 0.4f && (P.stable >= 3 || P.age >= 1.0f)) {
            PanelCacheCheck(true);
            PanelLockAnchor(eye);
        }
    }
    if (P.kind == kPanelTool) {
        // The tool ring follows only the player's own moves (the stick's moves and turns carry the room, and the
        // ring with it): once the eyes are more than 0.25 m from where it was placed (a step, a crouch), it glides
        // by as much - same height, distance and direction from the eyes. Never for a turn of the head (the eyes
        // move a few cm then), nor while a trigger drags in the window (a bullet).
        const ModUiPointerState& m = g_modUi;
        const bool drag = m.hand >= 0 && m.trigDown[m.hand] && !m.pressParked[m.hand];
        const Vec3 moved = eye - P.placeEye;
        if (Length(moved) > 0.25f && !drag) {
            const Pose from = P.gliding ? P.glideTo : P.pose;
            P.glideTo = { from.q, from.p + moved };
            P.placeEye = eye;
            P.gliding = true;
            static unsigned follows = 0;
            if (follows++ < 30)
                Log("Engine: mod panel follows the player (moved %.2f m) - the tool ring keeps its place in front of the eyes "
                    "(anchor %.0f %.0f; k %.5f; %s)", Length(moved), P.au, P.av, P.k, PanelPoseText(P.glideTo));
        }
    } else if (P.held < 0 && !P.gliding) {
        const float outside = PanelGazeOutside(eye, heading);
        const float d = Length(P.pose.p - eye);
        const bool away = outside > 10.0f || d < 0.3f || d > std::max(2.5f, P.place.dist + 1.0f);
        const ModUiPointerState& m = g_modUi;
        // (a trigger held in the window: a click or a drag - not one pulled beside it, ModUiPointer)
        const bool busy = P.rayOn || (m.hand >= 0 && m.trigDown[m.hand] && !m.pressParked[m.hand]);
        P.awayT = away && !busy ? P.awayT + dt : 0.0f;
        if (P.awayT > 0.5f) {
            P.awayT = 0;
            P.heading = heading;
            if (!P.placed) P.place.pitch = std::clamp(GazePitchDeg(), -50.0f, 25.0f);
            P.placeEye = eye;
            P.glideTo = PanelPoseAt(heading + P.place.yaw * kDeg, P.place.pitch * kDeg, P.place.dist, eye);
            if (Length(P.glideTo.p - P.pose.p) > 0.02f) {
                P.gliding = true;
                if (PanelLog())
                    Log("Engine: mod panel follows the player (gaze %.0f deg outside it, %.2f m away) - %s, yaw %+.0f pitch "
                        "%+.0f deg (anchor %.0f %.0f; k %.5f; %s)", outside, d,
                        P.place.yaw != 0 ? "back at its place from the gaze" : "back in front", P.place.yaw, P.place.pitch,
                        P.au, P.av, P.k, PanelPoseText(P.glideTo));
            }
        }
    }
    if (P.gliding) {
        const float a = 1.0f - std::exp(-dt / 0.12f);
        P.pose.p = P.pose.p + (P.glideTo.p - P.pose.p) * a;
        if (P.kind != kPanelTool) P.pose.q = PanelFacing(P.pose.p, eye);   // (the tool ring is moved, not turned)
        if (Length(P.glideTo.p - P.pose.p) < 0.005f) {
            P.pose = P.glideTo;
            P.gliding = false;
        }
    }
}

// Left eye: is a window shown for this stereo pair?
void ModUiPairStart()
{
    ModUiPointerState& m = g_modUi;
    BYTE* camObj = g_gameObj ? *(BYTE**)(g_gameObj + g_offGamePlayer) : nullptr;
    float blend;
    const bool was = m.pair;
    m.pairFrame = g_frameCounter;
    // (only in a synchronised stereo pair, i.e. in a level: the game's menus are shown on the
    // virtual screen, where input.cpp moves the real cursor)
    m.pair = m.ready && Cfg().modUiPointer && g_vrActive && g_rightPending && !Cfg().debugFovTest && camObj && m.shownFrame &&
             g_frameCounter - m.shownFrame <= 2 && MapPhase(camObj, blend) == 0 &&
             g_script.lastFrame != g_frameCounter && g_zoom.pair <= 1.0001f;
    static int logged = 0;   // (a mod showing its window every other frame: a few lines only)
    if (m.pair != was && logged < 40) {
        ++logged;
        Log("Engine: a mod's interactive window %s", m.pair ? "is shown - pointed at with the controllers" : "is closed");
    }
    if (!m.pair) {
        m.hand = -1;
        m.u = m.v = -1;
        m.parked = false;
        m.onWin[0] = m.onWin[1] = false;
        m.pressParked[0] = m.pressParked[1] = false;
    }
}

// Canvas point (canvas units) where the ray `aim` (recentred room space) meets the plane
// `dist` metres in front of the eyes, seen by the left eye (proj: its projection, w x h
// window pixels, V: canvas -> pixels). The left eye's view is its rotation in the room.
bool ModUiRayPoint(const Pose& aim, float dist, const float* proj, float w, float h, const float* V, float& u, float& v)
{
    const Vec3 mid = g_leftEyePos + Rotate(g_leftEyeRot, g_eyeOffset.p * (0.5f / std::max(0.1f, Cfg().worldScale)));
    const Vec3 f = Rotate(g_leftEyeRot, { 0, 0, -1 });
    const Vec3 d = Rotate(aim.q, { 0, 0, -1 });
    const float den = Dot(d, f);
    if (den < 0.05f) return false;   // along the plane, or away from it
    const float t = (dist - Dot(aim.p - mid, f)) / den;
    if (t <= 0) return false;        // the hand is beyond the plane
    const Vec3 e = Rotate(Conjugate(g_leftEyeRot), aim.p + d * t - g_leftEyePos);   // left eye view space
    if (e.z > -0.01f) return false;
    const float ax = proj[0] * (e.x / -e.z) - proj[8], ay = proj[5] * (e.y / -e.z) - proj[9];
    u = ((ax * 0.5f + 0.5f) * w - V[12]) / V[0];
    v = ((0.5f - ay * 0.5f) * h - V[13]) / V[5];
    return std::isfinite(u) && std::isfinite(v);
}

// Each eye, after its canvas shift: the cursor (found by the left eye) in this eye's pixels.
void ModUiPointer(int eye, const float* proj, float w, float h, const float* V)
{
    ModUiPointerState& m = g_modUi;
    if (!ModUiNow(eye) || V[0] <= 0 || V[5] <= 0) return;
    // (a menu shown as a floating panel, or the tool ring fixed in the room: the rays meet the panel -
    // within its content box + its frame band, where a click beside the window still closes it as on a
    // desktop; the cursor is drawn where the panel shows that canvas point in each eye)
    const bool panel = PanelSession() && g_panel.qFrame[eye] == g_frameCounter;
    if (eye == 0) {
        const float cw = w / V[0], ch = h / V[5];   // canvas size (UiWidth, UiHeight)
        const bool fresh = m.rayFrame == g_frameCounter;
        bool hit[2] = {}, press[2] = {};
        float hu[2] = {}, hv[2] = {};
        for (int i = 0; i < 2; ++i) {
            float t = 0;
            if (panel)
                hit[i] = fresh && m.ray[i] && PanelRay(m.aim[i], hu[i], hv[i], t) &&
                         PanelInBox(hu[i], hv[i], EngineModPanel::kBand);
            else
                hit[i] = fresh && m.ray[i] && ModUiRayPoint(m.aim[i], ModUiDistance(), proj, w, h, V, hu[i], hv[i]) &&
                         hu[i] > -0.05f * cw && hu[i] < 1.05f * cw && hv[i] > -0.05f * ch && hv[i] < 1.05f * ch;
            // (same thresholds as the mouse buttons in input.cpp: a press is seen by both at once)
            const bool down = !fresh ? m.trigDown[i] : m.trigDown[i] ? m.trigger[i] > 0.35f : m.trigger[i] > 0.55f;
            press[i] = down && !m.trigDown[i];
            m.trigDown[i] = down;
            m.onWin[i] = hit[i];   // (vr.cpp, OffLaserStop: that hand's laser stops on the window)
            // (a menu on its floating panel: a press begun with the ray beside it stays beside it until
            // released - held, it would otherwise bring the mouse back onto the last hovered widget)
            if (press[i]) m.pressParked[i] = panel && !hit[i];
            else if (!down) m.pressParked[i] = false;
        }
        if (panel) g_panel.rayOn = hit[0] || hit[1];   // (it is not brought back in front meanwhile)
        // The hand whose trigger was pulled last on a 2D interface points (m.pref: with offHandPointer
        // the pointer hand shared with our panels and the menus' screen, Input::PointerHand - at first
        // the dominant one). The other hand takes it only by pulling its trigger with its ray on the
        // canvas (that press is its first click, at its own ray); its ray alone on the canvas changes
        // nothing (as the player asked: the hands no longer swap the pointer by themselves). A hand now
        // on one of our own panels (or no longer tracked) lets go; a drag (the pointing hand's trigger
        // held) keeps its hand, also off the canvas. Its ray beside the canvas: the cursor stays (a menu
        // on its floating panel: the mouse is parked beside it, below).
        if (Cfg().offHandPointer) m.pref = Input::PointerHand();
        else if (m.pref < 0) m.pref = Cfg().leftHanded ? 0 : 1;
        const int cur = m.hand >= 0 && (!fresh || m.ray[m.hand]) ? m.hand : -1;
        const bool held = cur >= 0 && m.trigDown[cur] && !press[cur];
        // (with offHandPointer our panels and the menus' screen follow that hand too - unless the pointer
        // hand's trigger is held: Input::TakePointer refuses it, and the window keeps its hand)
        if (!held && !(press[m.pref] && hit[m.pref]))
            for (int i = 0; i < 2; ++i)
                if (press[i] && hit[i] && Input::TakePointer(i)) m.pref = i;
        const int hand = held ? cur : (!fresh || m.ray[m.pref]) ? m.pref : -1;
        static int logged = 0;   // (a hand at the canvas edge may come and go: a few lines only)
        if (hand != m.hand && logged < 30) {
            ++logged;
            Log("Engine: mod window pointer: %s", hand < 0 ? "no hand" : hand ? "right hand" : "left hand");
        }
        m.hand = hand;
        m.parked = false;
        if (panel && m.hand >= 0 && m.pressParked[m.hand] && m.trigDown[m.hand]) {
            // (its trigger was pulled while it pointed beside the window: that press stays there, with
            // no cursor, until released - even if the ray comes onto the panel meanwhile)
            m.parked = true;
        } else if (m.hand >= 0 && hit[m.hand]) {   // (otherwise it stays where it was: a drag off the canvas)
            m.u = std::clamp(hu[m.hand], 0.0f, cw);
            m.v = std::clamp(hv[m.hand], 0.0f, ch);
        } else if (panel && !held) {
            // (a menu on its floating panel, no ray on it - the panel is small in the view, the hand often
            // points elsewhere: the mouse goes beside the window, not on the button it last hovered. A
            // trigger then clicks outside the window, as on a desktop - Natural Phenomena closes. No
            // cursor is drawn.)
            m.parked = true;
        } else if (m.u < 0) {
            m.u = 0.5f * cw;   // (no hand points at the window yet: its centre, the same in both eyes)
            m.v = 0.5f * ch;
        }
        if (Cfg().debugLua) {
            static uint64_t since = 0;
            if (g_frameCounter - since >= 600) {
                Log("Engine: mod window pointer: hand %d at %.0f, %.0f (canvas %.0f x %.0f), rays %d/%d on it, "
                    "%u mouse reads given the cursor", m.hand, m.u, m.v, cw, ch, hit[0] ? 1 : 0, hit[1] ? 1 : 0, m.reads);
                since = g_frameCounter;
                m.reads = 0;
            }
        }
    }
    if (m.parked) {
        // (300 canvas units beyond the content box, on its roomier side - beyond its frame band too)
        const ModPanelState& P = g_panel;
        const float pu = P.hitOk ? (P.hit[0] > P.cw - P.hit[2] ? P.hit[0] - 300.0f : P.hit[2] + 300.0f) : -300.0f;
        const float pv = P.hitOk ? 0.5f * (P.hit[1] + P.hit[3]) : 0.5f * P.ch;
        m.px = V[0] * pu + V[12];
        m.py = V[5] * pv + V[13];
        m.mouseFrame = g_frameCounter;
        return;
    }
    if (m.u < 0) return;
    m.px = V[0] * m.u + V[12];
    m.py = V[5] * m.v + V[13];
    m.mouseFrame = g_frameCounter;
    if (!panel || !PanelProject(eye, m.u, m.v, m.cursor[eye][0], m.cursor[eye][1])) {
        m.cursor[eye][0] = m.px / w;
        m.cursor[eye][1] = m.py / h;
    }
    m.cursorFrame[eye] = g_frameCounter;
}

// room: eyeCam is the VR head's camera (the room yaw x the headset's eye pose, or the right eye
// moved from such a left one): mods' menus can be drawn on a panel fixed in the room (ModPanel).
void ApplyUiShift(const Pose& eyeCam, const float* proj, int eye, bool room)
{
    // (Not during the eye-sync test: it reads back the marker main.lua draws at the canvas corner.)
    if (!g_uiReady || !g_gameObj || !Cfg().hudFix || Cfg().debugSyncTest) return;
    float* P = (float*)(g_gameObj + g_offUiCam);   // pixels -> clip space
    float* V = P + 16;                              // canvas -> pixels
    // Only this frame's fresh camera, and only once.
    if (P[0] <= 0 || P[5] >= 0 || std::abs(P[12] + 1) > 1e-3f || std::abs(P[13] - 1) > 1e-3f) return;
    if (V[12] != 0 || V[13] != 0 || V[15] != 1) return;
    if (eye == 0) ModUiPairStart();   // (the left eye decides for its stereo pair)
    // (the floating panel of a mod's menu: its place for this pair - before the HUD's convergence below)
    if (eye == 0 && room && g_panel.ready && V[0] > 0 && V[5] > 0) PanelPairUpdate(2.0f / (P[0] * V[0]), -2.0f / (P[5] * V[5]));
    Vec3 d = Rotate(Conjugate(eyeCam.q), g_uiDir);   // in this eye's view space
    float cx, cy;
    if (d.z < -0.05f) {
        float tx = d.x / -d.z, ty = d.y / -d.z;
        if (Cfg().hudDistance > 0.1f) {
            // Seen at this distance instead of infinity (left eye: a bit to the right).
            float ipd = Length(g_eyeOffset.p) / std::max(0.1f, Cfg().worldScale);
            // (divided by the mod camera's zoom: the projection below magnifies it, and the
            // HUD must stay at this distance, not come zoom times closer: ModZoom)
            tx += (eye ? -0.5f : 0.5f) * ipd / (Cfg().hudDistance * g_zoom.pair);
        }
        if (ModUiNow(eye) && !PanelSession()) {
            // A mod's interactive window: the canvas is seen at modUiDistance instead of
            // infinity / hudDistance (ModUiPointer; never zoomed then: g_zoom.pair is 1). (A menu
            // drawn on a floating panel, or the tool ring fixed in the room: the HUD stays where it is.)
            const float ipd = Length(g_eyeOffset.p) / std::max(0.1f, Cfg().worldScale);
            tx = d.x / -d.z + (eye ? -0.5f : 0.5f) * ipd / ModUiDistance();
        }
        // The engine's (OpenGL) convention: clip.x = m0 x + m8 z, clip.w = -z.
        cx = std::clamp(proj[0] * tx - proj[8], -3.0f, 3.0f);
        cy = std::clamp(proj[5] * ty - proj[9], -3.0f, 3.0f);
    } else {
        // Behind the eye (looking away from a mod camera's axis): out of view on that side.
        float l = std::sqrt(d.x * d.x + d.y * d.y) + 1e-6f;
        cx = 3.0f * d.x / l;
        cy = 3.0f * d.y / l;
    }
    if (g_dolly.uiDist > 0) {
        // Zoom by approach: the canvas centre on the point the mod aims at, so its crosshair
        // is seen on the target, at the target's distance, by both eyes (DollyTravel; in
        // place of the direction / HUD distance above).
        const Vec3 a = Rotate(Conjugate(eyeCam.q), g_dolly.aim - eyeCam.p);
        if (a.z < -0.05f * Length(a)) {
            cx = std::clamp(proj[0] * (a.x / -a.z) - proj[8], -3.0f, 3.0f);
            cy = std::clamp(proj[5] * (a.y / -a.z) - proj[9], -3.0f, 3.0f);
        } else {
            float l = std::sqrt(a.x * a.x + a.y * a.y) + 1e-6f;
            cx = 3.0f * a.x / l;
            cy = 3.0f * a.y / l;
        }
    }
    float w = 2.0f / P[0], h = -2.0f / P[5];   // screen size, pixels (y down)
    V[12] = cx * 0.5f * w;
    V[13] = -cy * 0.5f * h;
    // (every VR room frame, also before any window: one opening in this frame is on its panel at once)
    if (room && g_panel.ready) PanelBuildQ(eye, eyeCam, proj, w, h, V);
    AimUiOffset(eyeCam, proj, eye, cx, cy, w, h, V);
    ModUiPointer(eye, proj, w, h, V);
}

void __fastcall Hook_CamSet(void* cam, const float* pos, const float* rot)
{
    BYTE* player = g_gameObj ? *(BYTE**)(g_gameObj + g_offGamePlayer) : nullptr;
    // pauseWide: a paused level drawn for a 16:9 screen (WideDecide) gets the game's own view made 16:9 - its
    // vertical field of view, the horizontal one a 16:9 screen has, centred: the desktop game's picture - instead
    // of the eye's (off-centre, nearly square) projection. Stretched over the window like the 2D canvas; the 16:9
    // quad shows it with its proportions. (setPerspective rebuilds it every frame: nothing to undo.)
    if (WideNow() && player && (const BYTE*)pos == player + g_offCamPos && (const BYTE*)rot == player + g_offCamRot) {
        float* proj = (float*)cam;
        if (proj[0] > 0 && proj[5] > 0) proj[0] = proj[5] * (9.0f / 16.0f);
        Real_CamSet(cam, pos, rot);
        return;
    }
    if (g_vrActive && player && (const BYTE*)pos == player + g_offCamPos && (const BYTE*)rot == player + g_offCamRot) {
        float blend = 1;
        int mapPhase = MapPhase(player, blend);
        if (mapPhase == 0) g_map.on = false;
        // A mod drives the camera this frame (SetCameraTransform): see ScriptView.
        bool modCam = g_scriptMode && *(int*)player == g_scriptMode && !mapPhase;
        float modTanV = 0;   // the mod camera's own field of view, tan(vertical / 2), before it is replaced
        if ((mapPhase || modCam) && g_vrFov > 0) {
            // The game forces a 90 degree field of view on the map, and a mod camera
            // its own (the AC130's zoom): use the VR one (same projection as the
            // normal view, aspect ratio kept).
            float* proj = (float*)cam;
            if (proj[0] > 0 && proj[5] > 0) {
                if (modCam) modTanV = 1.0f / proj[5];   // (its zoom: see ModZoom)
                float aspect = proj[5] / proj[0];
                float f = 1.0f / (std::tan(g_vrFov * 3.14159265f / 360.0f) * 0.5625f);
                proj[5] = f;
                proj[0] = f / aspect;
            }
        }
        if (g_offCenter) ApplyEyeProjection((float*)cam, g_rightThisFrame ? 1 : 0);
        if (g_rightThisFrame && g_leftCamValid) {
            ApplyModZoom((float*)cam, g_zoom.pair);   // (the left eye's zoom: the same in both eyes)
            Pose offset = g_eyeOffset;
            offset.p = offset.p * g_curK;
            Pose c = Mul(g_leftCam, offset);
            if (Cfg().debugLua) {
                // How far the game's own right-eye camera drifted from the stereo pair.
                static float maxDrift = 0;
                static int n = 0;
                Vec3 d = Vec3{ pos[0], pos[1], pos[2] } - c.p;
                maxDrift = std::max(maxDrift, Length(d));
                if (++n == 600) {
                    Log("Engine: right-eye camera corrected, max drift %.4f m over 600 frames", maxDrift);
                    maxDrift = 0;
                    n = 0;
                }
            }
            if (Cfg().debugLua) ToolVsCamera(c, 1);
            ApplyUiShift(c, (const float*)cam, 1, g_leftCamRoom && g_curK == 1.0f);
            const float p[3] = { c.p.x, c.p.y, c.p.z };
            const float q[4] = { c.q.x, c.q.y, c.q.z, c.q.w };
            Real_CamSet(cam, p, q);
            return;
        }
        if (g_leftThisFrame) {
            g_leftCam = { { rot[0], rot[1], rot[2], rot[3] }, { pos[0], pos[1], pos[2] } };
            g_leftCamRoom = false;   // (set below: the VR head's camera, not the map's / a mod's)
            g_zoom.pair = 1;   // (set by ScriptView in a mod camera view)
            g_dolly.uiDist = 0;   // (likewise: DollyTravel)
            if (Cfg().debugLua || g_perfTrace) {
                // Smoothness of the camera, one sample per stereo pair: the change of
                // the per-pair displacement ("jerk") shows shaking. (perfTrace too: the stress
                // test's validity check - the camera stays still, no knock-back - without debugLua.)
                static Vec3 prevP{}, prevD{};
                static int n = 0;
                static float maxJerk = 0, maxJerkY = 0, sumD = 0;
                static char pattern[400];
                static int plen = 0;
                Vec3 p{ pos[0], pos[1], pos[2] };
                Vec3 d = p - prevP;
                if (n > 0 && Length(d) < 1.0f) {
                    Vec3 j = d - prevD;
                    maxJerk = std::max(maxJerk, Length(j));
                    maxJerkY = std::max(maxJerkY, std::abs(j.y));
                    sumD += Length(d);
                    if (plen < 360) plen += snprintf(pattern + plen, sizeof(pattern) - plen, "%.0f ", Length(d) * 1000);
                }
                prevD = d;
                prevP = p;
                if (++n % 300 == 0) {
                    Log("Engine: camera motion over 300 pairs: mean step %.1f mm, max jerk %.1f mm (vertical %.1f mm); steps mm: %s",
                        sumD / 299 * 1000, maxJerk * 1000, maxJerkY * 1000, pattern);
                    maxJerk = maxJerkY = sumD = 0;
                    plen = 0;
                    pattern[0] = 0;
                }
            }
            g_curK = 1;
            if (modCam && Cfg().modCameraVr && !g_camNearPlayer) {
                // A mod's camera away from the player (AC130 plane view, drone, cinematic).
                ScriptView(g_leftCam, modTanV);
                ApplyModZoom((float*)cam, g_zoom.pair);   // (before ApplyUiShift: the crosshair follows)
                g_uiDir = Rotate(Quat{ rot[0], rot[1], rot[2], rot[3] }, { 0, 0, -1 });   // the mod's aim axis
                // (mods' menu panels too: a panel is placed in the room (head-tracking space) and Q
                // only uses the eyes' room poses, so it is right in a mod camera's view as well -
                // Natural Phenomena shows its weather menu over an aerial view of its own; not while
                // the view is zoomed: the image is then not the eyes' plain view)
                g_leftCamRoom = g_zoom.pair <= 1.0001f;
                ApplyUiShift(g_leftCam, (const float*)cam, 0, g_leftCamRoom);
                const float p[3] = { g_leftCam.p.x, g_leftCam.p.y, g_leftCam.p.z };
                const float q[4] = { g_leftCam.q.x, g_leftCam.q.y, g_leftCam.q.z, g_leftCam.q.w };
                g_leftCamValid = true;
                Real_CamSet(cam, p, q);
                return;
            }
            if (g_anchorFrame == g_frameCounter && g_anchorFrame) {
                // The view turns with the head only: room yaw in the world x eye rotation.
                g_leftCam.q = AxisAngle({ 0, 1, 0 }, g_anchorYaw * 3.14159265f / 180.0f) * g_leftEyeRot;
                if (mapPhase) MapCamera(player, mapPhase, blend, g_leftCam);
                else g_lastViewPos = g_leftCam.p;
                if (Cfg().debugLua) ToolVsCamera(g_leftCam, 0);
                g_uiDir = Rotate(g_leftCam.q, { 0, 0, -1 });   // the HUD straight ahead of the head
                g_leftCamRoom = !mapPhase;
                ApplyUiShift(g_leftCam, (const float*)cam, 0, g_leftCamRoom);
                const float p[3] = { g_leftCam.p.x, g_leftCam.p.y, g_leftCam.p.z };
                const float q[4] = { g_leftCam.q.x, g_leftCam.q.y, g_leftCam.q.z, g_leftCam.q.w };
                g_leftCamValid = true;
                Real_CamSet(cam, p, q);
                return;
            }
            g_leftCamValid = true;
            g_uiDir = Rotate(g_leftCam.q, { 0, 0, -1 });
            ApplyUiShift(g_leftCam, (const float*)cam, 0, false);   // (not the room's view: no panel this pair)
        }
    }
    Real_CamSet(cam, pos, rot);
}

void InitMapView()
{
    // Game draw, map field of view: cmp r8d,7 ; jne ; movss xmm0,[rdx+blend] ; jmp ; cmp dword [rdx+prev],7
    BYTE* a = Scan("41 83 F8 07 75 ?? F3 0F 10 82 ?? ?? 00 00 EB ?? 83 7A ?? 07");
    // Camera::setMode, map: mov dword [rdi+dist],60.0 ; unpcklps xmm0,xmm1 ; movsd [rdi+pos],xmm0
    BYTE* b = Scan("C7 87 ?? ?? 00 00 00 00 70 42 0F 14 C1 F2 0F 11 87 ?? ?? 00 00");
    // Map target from a point: cmp byte [rcx+X],0 ; movsd xmm0,[rdx] ; movsd [rcx+target],xmm0 ; mov eax,[rdx+8] ; mov [rcx+target+8],eax
    BYTE* c = Scan("80 B9 ?? ?? 00 00 00 F2 0F 10 02 F2 0F 11 81 ?? ?? 00 00 8B 42 08 89 81");
    if (!a || !b || !c || !g_offGamePlayer) {
        Log("Engine: level map not recognised (%p %p %p) - shown on the virtual screen", a, b, c);
        return;
    }
    g_offCamBlend = U32(a + 10);
    g_offCamPrevMode = a[18];
    g_offMapDist = U32(b + 2);
    g_offMapPos = U32(b + 17);
    g_offMapTarget = U32(c + 15);
    if (U32(c + 24) != g_offMapTarget + 8) {
        Log("Engine: unexpected map target layout - level map shown on the virtual screen");
        return;
    }
    g_mapReady = true;
    Log("Engine: 3D level map ready (blend %x, prev %x, target %x, dist %x, pos %x)", g_offCamBlend, g_offCamPrevMode,
        g_offMapTarget, g_offMapDist, g_offMapPos);
}

void InitScriptView()
{
    // Game draw, field of view of a script camera:
    //   mov rdx,[rsi+60h] ; mov r8d,[rdx] ; cmp r8d,8 ; jne ; movss xmm0,[rdx+2C4h] ; comiss xmm0,xmm7
    BYTE* p = Scan("48 8B 56 ?? 44 8B 02 41 83 F8 ?? 75 ?? F3 0F 10 82 ?? ?? 00 00 0F 2F C7");
    if (!p || !g_offGamePlayer || p[3] != g_offGamePlayer) {
        Log("Engine: mod cameras not recognised (%p) - seen with the head's rotation only", p);
        return;
    }
    g_scriptMode = p[10];
    Log("Engine: mod cameras shown in VR (camera mode %d)", g_scriptMode);
}

void InitUiCamera()
{
    // Game draw, right after the render camera (copy of the UI camera to the 2D renderer):
    //   mov rax,[rsi+30h] ; movups xmm0,[rsi+ui] ; movups [rax+8],xmm0 ; movups xmm1,[rsi+ui+10h] ; movups [rax+18h],xmm1
    BYTE* a = Scan("48 8B 46 30 0F 10 86 ?? ?? 00 00 0F 11 40 08 0F 10 8E ?? ?? 00 00 0F 11 48 18");
    // Game::frame, every frame (rebuilds it, window width x height pixels):
    //   lea rcx,[r12+ui] ; movss xmm8,[1.0] ; movss [rsp+20h],xmm8 ; movss xmm3,[-1.0] ; mov r8d,[r12+4] ;
    //   mov edx,[r12] ; call Camera::setOrtho
    BYTE* b = Scan("49 8D 8C 24 ?? ?? 00 00 F3 44 0F 10 05 ?? ?? ?? ?? F3 44 0F 11 44 24 20 F3 0F 10 1D ?? ?? ?? ?? "
                   "45 8B 44 24 04 41 8B 14 24 E8");
    // The render camera call (see InitCameraHook): the copy must come after it, in the same function.
    BYTE* c = Scan("48 8B 56 ?? 4C 8D 82 ?? ?? 00 00 48 81 C2 ?? ?? 00 00 48 8D 4D ?? E8 ?? ?? ?? ??");
    if (!a || !b || !c || U32(a + 7) != U32(b + 4) || U32(a + 18) != U32(a + 7) + 0x10 || a < c || a - c > 0x1000) {
        Log("Engine: 2D interface camera not recognised (%p %p %p) - HUD left as the game draws it", a, b, c);
        return;
    }
    g_offUiCam = U32(a + 7);
    g_uiReady = true;
    // (the 2D renderer and where its copy of the camera goes: InitModPanel)
    g_offRenderer = a[3];
    g_offRendererProj = a[14];
    Log("Engine: 2D interface centred in each eye (UI camera game+%x)", g_offUiCam);
}

// Which UI draws are crosshairs (see AimUiOffset). Every Ui* drawing function
// (UiImage, UiImageBox, UiText, and UiRect / UiRoundedRect / UiCircle and their
// outlines through one helper) builds the quad's canvas transform M (the
// UiTranslate / UiScale / UiRotate state times the alignment), asks this function
// whether the rect lo..hi under M is on screen, then draws with that same M
// (Teardown 2.0.1: 0x4d1f90). Moving M here moves the drawing; world markers placed
// with UiWorldToPixel are told apart below. (UiImageAlpha / UiImageOverlayAlpha
// build their draw constants before the test: they are not moved.)
// A quad is a crosshair when it is small and its centre is near the canvas centre,
// except:
//   - inside the UiPush scope where its script called UiWorldToPixel: a world marker
//     (e.g. the game's "interact" icon on the handle you look at) stays exact;
//   - while a script shows a window with UiMakeInteractive: its buttons stay under
//     the mouse cursor;
//   - the tests called by UiImageAlpha / UiImageOverlayAlpha (only the test would
//     move: their quad, left in the centre, could then be culled) and UiTextInput
//     (stays under the mouse), told apart by their return address (g_uiFixedCalls).
using PFN_UiRectVisible = bool(__fastcall*)(BYTE* ui, float* m, const float* lo, const float* hi);
PFN_UiRectVisible Real_UiRectVisible = nullptr;
PFN_LuaFn Real_UiWorldToPixel = nullptr, Real_UiPop = nullptr, Real_UiMakeInteractive = nullptr;
uint32_t g_offScriptUi = 0;     // script -> its UI context                          (0x418)
uint32_t g_offUiDepth = 0;      // UI context -> UiPush depth (0 at each draw() start) (0xac08)
uint64_t g_uiInteractiveFrame = 0;
constexpr float kAimUiZone = 160.0f;     // canvas units (1920 wide) around the centre
constexpr float kAimUiMaxSize = 360.0f;  // larger quads (panels, full-screen effects) never move
struct UiWorldMark { BYTE* ui = nullptr; int depth = 0; uint64_t frame = 0; };
UiWorldMark g_uiWorldMarks[32];         // scripts that placed a world marker this frame

UiWorldMark* WorldMark(BYTE* ui, bool create)
{
    UiWorldMark* freeSlot = nullptr;
    for (UiWorldMark& m : g_uiWorldMarks) {
        if (m.frame == g_frameCounter && m.ui == ui) return &m;
        if (m.frame != g_frameCounter && !freeSlot) freeSlot = &m;
    }
    return create ? freeSlot : nullptr;
}

int UiDepth(BYTE* ui) { return *(int*)(ui + g_offUiDepth); }

// World markers of resized mod tools and of interactive windows (UiWorldToPixel). A mod
// places markers on its tool from the tool as it made it: THE EVERYTHING REVOLVER draws
// its ammo ring and chambers around LnLGetBoneWorldTransform(cylinder) = tool body x
// unscaled bone. The tool is drawn scaled by s about its muzzle M (ScaleModTool): the
// mod's point p is drawn at M + s (p - M), so the cylinder bone was (1 - s) x 1.07 m =
// 0.76 m (s = 0.29, measured live) behind the drawn cylinder - behind the player's eyes
// with the gun held in front: the ring was mirrored, in a different place in each eye,
// and its chambers had a negative size. A point inside the box of the mod's unscaled
// tool is projected where it is drawn, and its distance (third result) is the drawn
// part's divided by s: the one the mod expects for its tool as made (it divides sizes by
// it). Only for the tool's own script (the one calling SetToolTransform, ToolScript): a
// marker another script puts on something near the player's face, inside that box, is
// left alone. In the window of a script showing an interactive one (ModUiPointer) every
// marker is projected from between the eyes onto the canvas plane: the same canvas point
// in both eyes - except those on its own held tool, seen as on a desktop screen and
// centred (ToolRing, modUiCenterTool). The binding projects with the 3D camera (game+0x240:
// projection, then the world -> eye view at +0x40, column-major); its distance is the depth
// along the camera controller's axis (position +0x1d0, rotation +0x1dc). For the call: the view
// translation shows M + s (p - M) from the chosen eye point, divided by s (the same
// pixel); the projection's m8 gets the canvas plane's shift for this eye (as
// ApplyUiShift); the controller's position becomes M + (c - M) / s. All restored right
// after. Between its projection and its way back through the UI camera, the binding has
// the pixel bent like the game's barrel distortion (0x74a10, only called from there: radially
// about the window centre, when options.gfx.barrel - game+0x388 - is on and the map is not
// shown). In the window of a script showing an interactive one that flag is off for the
// call: the converged canvas puts the same canvas point at a different window pixel in
// each eye, which the bend would move differently (two rings, off the centre).
uint32_t g_off3dCam = 0;              // Game -> 3D camera UiWorldToPixel projects with (0x240)
uint32_t g_offUiPixelBend = 0;        // Game -> options.gfx.barrel (0x388), the binding's pixel bend
uint32_t g_offUiTransform = 0;        // UI context -> its transform (UiTranslate...: 0x2f8), undone last by the binding
uint32_t g_offRetCount = 0;           // (debugLua) Lua return object -> count of results (+8); its lua_State first
uint32_t g_offLuaTop = 0;             // (debugLua) lua_State -> top (+0x10): 16-byte values, number type 3
using PFN_ArgVec = void(__fastcall*)(void* args, int index, float* out, int count);
PFN_ArgVec g_luaArgVec = nullptr;     // the engine's ArgVec (+631a60): a Lua vector argument
unsigned g_worldPixelMoved[2] = {};   // diagnostics (debugLua): tool markers, window markers

// Box of the held mod tool `tool` as its mod made it (tool body coordinates), measured
// again every 60 frames (a mod moving parts - the revolver's cylinder swinging out - stays
// within the box's 5 cm margin meanwhile). Resized or not (ToolRing also uses it at s = 1),
// so any voxel size counts; shapes sent far away by their mod are hidden ones.
bool HeldToolBox(BYTE* tool, Vec3& lo, Vec3& hi)
{
    static uint64_t frame = 0;
    static BYTE* body = nullptr;
    static Vec3 l, h;
    static int n = 0;
    if (g_frameCounter - frame >= 60 || body != tool) {
        frame = g_frameCounter;
        body = tool;
        l = { 1e9f, 1e9f, 1e9f };
        h = { -1e9f, -1e9f, -1e9f };
        n = 0;
        if (body)
            ForToolShapes(body, [&](BYTE* sh) {
                Vec3 pos;
                float scale;
                ShapeOriginal(sh, pos, scale);
                const int* size = *(const int* const*)(sh + kShapeVox);
                const float* lt = (const float*)(sh + kShapeLocal);
                Quat q{ lt[3], lt[4], lt[5], lt[6] };
                if (!(scale > kMinVoxel) || !std::isfinite(scale) || size[0] <= 0 || size[1] <= 0 || size[2] <= 0 ||
                    !(Length(pos) < kHiddenDist))
                    return;
                for (int k = 0; k < 8; ++k) {
                    Vec3 c{ (k & 1) ? size[0] * scale : 0.0f, (k & 2) ? size[1] * scale : 0.0f, (k & 4) ? size[2] * scale : 0.0f };
                    Vec3 p = pos + Rotate(q, c);
                    l = { std::min(l.x, p.x), std::min(l.y, p.y), std::min(l.z, p.z) };
                    h = { std::max(h.x, p.x), std::max(h.y, p.y), std::max(h.z, p.z) };
                }
                ++n;
            });
    }
    lo = l;
    hi = h;
    return n > 0;
}

// The held mod tool's script: the one that calls SetToolTransform (Hook_SetToolTransform). A
// tool whose script does not call it: every script (the box test alone).
bool ToolScript(void* ctx)
{
    return !g_toolScriptCtx || g_frameCounter - g_toolScriptFrame > 2 || ctx == g_toolScriptCtx;
}

// The desktop field of view: options.xml <gfx><fov value=...> (horizontal, for a 16:9
// screen like SetCameraFov; 90 when it cannot be read). Mods size what they draw from it
// (THE EVERYTHING REVOLVER: chambers of 32 / distance canvas units at 90 degrees, 20 /
// distance at 120). Read again only when the file changed since (the game rewrites it when
// its options change); asked once per window session (ToolRing).
float DesktopFov()
{
    static float fov = 90.0f;
    static bool read = false;
    static FILETIME stamp{};
    static std::wstring path;
    if (path.empty()) {
        wchar_t dir[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
        if (!n || n >= MAX_PATH) return fov;
        path = std::wstring(dir) + L"\\Teardown\\options.xml";
    }
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) ||
        (read && CompareFileTime(&a.ftLastWriteTime, &stamp) == 0))
        return fov;
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string xml = ss.str();
    if (xml.empty()) return fov;   // (being rewritten: read next time)
    stamp = a.ftLastWriteTime;
    const size_t npos = std::string::npos;
    const size_t g = xml.find("<gfx>"), e = g == npos ? npos : xml.find("</gfx>", g);
    const size_t v = g == npos ? npos : xml.find("<fov value=\"", g);
    float value = v != npos && v < e ? std::strtof(xml.c_str() + v + 12, nullptr) : 90.0f;
    value = std::isfinite(value) && value > 1.0f ? std::clamp(value, 40.0f, 140.0f) : 90.0f;
    if (!read || value != fov) Log("Engine: desktop field of view %.0f degrees (options.xml)", value);
    read = true;
    fov = value;
    return fov;
}

// THE EVERYTHING REVOLVER's ammo menu at the centre of the view (modUiCenterTool). Its
// window draws the ring of chambers where the mod sees its cylinder: UiWorldToPixel of the
// cylinder bone and of each chamber bone of its tool (tool body x bone, LnL). With the tool
// in the VR hand these points were projected where the gun is drawn (WorldPixelView): the
// ring followed the hand, turned, squashed or mirrored with the wrist - and that hand often
// points at it too. Now, while the tool's own script shows an interactive window, a marker
// on the held tool is projected as the mod made it for a desktop screen: from the camera
// its SetToolTransform is relative to, E = tool body x inverse(flat) (flat = the mod's
// desktop pose of the tool, g_scriptToolFlat, latched by the pair's left eye), with the
// desktop field of view: eye point = flat x (point in the tool body), whatever the hand
// does, and its distance (third result) is that camera's depth: chambers as big as on the
// desktop. They are all shifted together so that their centroid is the canvas centre: the anchor
// of the window's panel fixed in the room, straight ahead at eye height (modUiToolFixed, on by
// default: PanelToolPlace - the markers projected before UiMakeInteractive too, as the revolver
// does), or else the point the converged canvas shows straight ahead at modUiDistance in both
// eyes (ApplyUiShift: the ring stays at the centre of the view when the head moves). Either
// way the same canvas point in
// both eyes. The shift follows the pair's first marker (the cylinder) at once - the ring as
// a whole stays put through the mod's sway and its choppy 24 Hz pose steps - plus the
// centroid's offset from that marker, smoothed (0.1 s) over the previous pairs. The pointer
// is the window's (ModUiPointer: the rays on its panel, or on the head-locked canvas). The window's other
// markers (its crosshair point far ahead...) keep the projection from between the eyes.
struct ToolRing
{
    uint64_t pair = 0;        // left-eye frame of the stereo pair (g_modUi.pairFrame) the values below are for
    BYTE* body = nullptr;     // the tool body they are for
    Pose flat;                // the mod's desktop pose of its tool, latched by the pair's left eye
    float f = 0;              // desktop focal length, canvas units (half the canvas width / tan(fov / 2))
    int n = 0;                // markers of the pair so far (left eye)
    float first[2] = {}, sum[2] = {};   // their desktop offsets (canvas units from the screen centre, y down)
    float anchor[2] = {};     // desktop offset shown at the canvas centre in this pair
    float delta[2] = {};      // centroid - first marker, smoothed over the pairs
    bool deltaValid = false;
    unsigned sessions = 0;    // (log lines)
    // Diagnostics (debugLua): both eyes of one pair every 300 frames, logged at the next pair:
    // what the binding returned to the mod (WorldPixelView::Check), against the point wanted.
    uint64_t diagPair = 0, diagLast = 0;
    struct Eye { int n = 0, unread = 0; float first[2] = {}, sum[2] = {}, depth = 0, err = 0, depthErr = 0; } diag[2];
    float centre[2] = {};
    int sizes[4] = {};        // window (game+0 / +4) and script (ui+0x10 / +0x14) sizes, pixels
    int bend = -1;            // options.gfx.barrel before the calls (switched off for them; -1: flag not found)
} g_toolRing;

// Desktop offset of a point of the tool body (canvas units from the screen centre, y down)
// and its depth, seen by the mod's desktop camera. False behind that camera.
bool ToolRingDesk(const Vec3& p, float d[2], float& depth)
{
    const ToolRing& r = g_toolRing;
    const Vec3 e = r.flat.p + Rotate(r.flat.q, p);
    if (!(e.z < -0.01f)) return false;
    depth = -e.z;
    d[0] = r.f * e.x / depth;
    d[1] = -r.f * e.y / depth;
    return std::isfinite(d[0]) && std::isfinite(d[1]);
}

// Left eye, first marker on the held tool in a stereo pair: the previous pair's centroid
// (smoothed), this pair's desktop pose of the tool; after a gap (window closed, another
// tool), a new session.
void ToolRingPair(BYTE* body, float halfWidth)
{
    ToolRing& r = g_toolRing;
    if (r.diagPair && r.diagPair == r.pair) {
        // (debugLua) The canvas points the binding returned to the mod (Check): both eyes should
        // give the same ones, their centroid on the centre.
        auto mean = [](const ToolRing::Eye& e, int k) { return e.n ? e.sum[k] / e.n : 0.0f; };
        const ToolRing::Eye& L = r.diag[0];
        const ToolRing::Eye& R = r.diag[1];
        Log("Engine diag: mod tool ring - left eye %d markers, first %.1f %.1f, centroid %.1f %.1f (depth %.2f m); "
            "right eye %d markers, first %.1f %.1f, centroid %.1f %.1f; canvas centre %.0f %.0f; returned values off "
            "the wanted ones by %.2f / %.2f units, depth %.3f / %.3f m at most (%d / %d not read); desktop focal %.0f; "
            "window %dx%d, script %dx%d; barrel distortion flag %d", L.n, L.first[0], L.first[1], mean(L, 0),
            mean(L, 1), L.depth, R.n, R.first[0], R.first[1], mean(R, 0), mean(R, 1), r.centre[0], r.centre[1],
            L.err, R.err, L.depthErr, R.depthErr, L.unread, R.unread, r.f, r.sizes[0], r.sizes[1], r.sizes[2],
            r.sizes[3], r.bend);
        r.diagPair = 0;
    }
    const bool session = r.n > 0 && r.body == body && g_modUi.pairFrame - r.pair <= 8;
    if (session) {
        const float c[2] = { r.sum[0] / r.n - r.first[0], r.sum[1] / r.n - r.first[1] };
        const float a = r.deltaValid ? 1.0f - std::exp(-std::clamp(g_lastLeftDt, 0.0f, 0.1f) / 0.1f) : 1.0f;
        for (int k = 0; k < 2; ++k) r.delta[k] += (c[k] - r.delta[k]) * a;
        r.deltaValid = true;
    } else {
        // (the desktop field of view may have changed in the game's options meanwhile)
        r.deltaValid = false;
        r.delta[0] = r.delta[1] = 0;
        const float fov = DesktopFov();
        r.f = halfWidth / std::tan(0.5f * fov * kDeg);
        if (r.sessions < 20) {
            ++r.sessions;
            Log("Engine: mod window: the markers on the held tool '%s' are drawn as on a desktop screen (field of "
                "view %.0f), centred %s", g_scriptToolName.c_str(), fov,
                PanelSession() && g_panel.kind == kPanelTool ? "on its panel fixed in the room" : "in the view");
        }
    }
    r.pair = g_modUi.pairFrame;
    r.body = body;
    r.flat = { Normalize(g_scriptToolFlat.q), g_scriptToolFlat.p };
    r.n = 0;
    r.sum[0] = r.sum[1] = 0;
    if (Cfg().debugLua && g_modUi.pairFrame - r.diagLast >= 300) {
        r.diagPair = r.diagLast = g_modUi.pairFrame;
        r.diag[0] = r.diag[1] = {};
    }
}

void PanelEnd(const char* why);   // (below: the floating panel's scope ends)

// Is the marker UiWorldToPixel was called with (args) on the mod tool held in the hand, drawn by that
// tool's own script (SetToolTransform this frame or the one before)? The tool box test of ToolRing.
bool HeldToolMarker(void* ctx, void* args)
{
    if (!ctx || ctx != g_toolScriptCtx || g_frameCounter - g_toolScriptFrame > 2 || g_scriptToolName.empty() ||
        !g_luaArgVec)
        return false;
    BYTE* pl = LocalPlayer();
    BYTE* body = pl ? *(BYTE**)(pl + kPlayerToolBody) : nullptr;
    if (!body || body != g_scaledBody || *(uint16_t*)(body + kEntityType) != 1) return false;
    float a[3] = {};
    g_luaArgVec(args, 0, a, 3);
    const float* b = (const float*)(body + 0x40);   // tool body: world position, then rotation
    const Pose bt{ Normalize({ b[3], b[4], b[5], b[6] }), { b[0], b[1], b[2] } };
    const Vec3 p = Rotate(Conjugate(bt.q), Vec3{ a[0], a[1], a[2] } - bt.p);
    Vec3 lo, hi;
    const float pad = 0.05f;
    return HeldToolBox(body, lo, hi) && p.x > lo.x - pad && p.x < hi.x + pad && p.y > lo.y - pad && p.y < hi.y + pad &&
           p.z > lo.z - pad && p.z < hi.z + pad;
}

struct WorldPixelView
{
    float* t = nullptr;      // the 3D camera's view translation
    float* m8 = nullptr;     // its projection's horizontal shift
    float* c = nullptr;      // the camera controller's position
    float* cam3d = nullptr;  // the whole 3D camera: projection, then view (ToolRing)
    float* rot = nullptr;    // the camera controller's rotation (ToolRing)
    int* bend = nullptr;     // the barrel distortion flag (interactive windows)
    float saveT[3] = {}, saveM8 = 0, saveC[3] = {}, saveCam[32] = {}, saveRot[4] = {};
    int saveBend = 0;
    // (debugLua, ToolRing) the eye whose result is checked after the call (-1: none), the
    // canvas point and depth wanted, the UI context
    int check = -1;
    float want[3] = {};
    BYTE* checkUi = nullptr;

    // A marker on the held mod tool, drawn by its own script while that script shows an
    // interactive window: as on a desktop screen, centred (ToolRing). False when it is not
    // one, or when the right eye has no anchor from the left one: the path below.
    bool SetDesktop(BYTE* ui, void* ctx, void* args, int eye)
    {
        ToolRing& r = g_toolRing;
        BYTE* pl = LocalPlayer();
        BYTE* body = pl ? *(BYTE**)(pl + kPlayerToolBody) : nullptr;
        BYTE* camObj = *(BYTE**)(g_gameObj + g_offGamePlayer);
        // (a mod tool in the hand - its desktop pose known - placed by this script this frame or the one before)
        if (!ui || !g_uiReady || !camObj || !g_offCamPos || !g_offCamRot || !body || body != g_scaledBody ||
            *(uint16_t*)(body + kEntityType) != 1 || g_scriptToolName.empty() || ctx != g_toolScriptCtx ||
            g_frameCounter - g_toolScriptFrame > 2)
            return false;
        float* U = (float*)(g_gameObj + g_offUiCam);   // UI camera: pixels -> clip, then canvas -> pixels (+16)
        if (U[0] <= 0 || U[5] >= 0 || U[16] <= 0 || U[21] <= 0) return false;
        float a[3] = {};
        g_luaArgVec(args, 0, a, 3);
        const float* b = (const float*)(body + 0x40);   // tool body: world position, then rotation
        const Pose bt{ Normalize({ b[3], b[4], b[5], b[6] }), { b[0], b[1], b[2] } };
        const Vec3 w{ a[0], a[1], a[2] };
        const Vec3 p = Rotate(Conjugate(bt.q), w - bt.p);   // in the tool body
        Vec3 lo, hi;
        const float pad = 0.05f;
        if (!HeldToolBox(body, lo, hi) || !(p.x > lo.x - pad && p.x < hi.x + pad && p.y > lo.y - pad &&
                                            p.y < hi.y + pad && p.z > lo.z - pad && p.z < hi.z + pad))
            return false;
        const float W = 2.0f / U[0], H = -2.0f / U[5];   // window, pixels
        const float cw = W / U[16], ch = H / U[21];      // canvas (UiWidth, UiHeight)
        if (eye == 0 && r.pair != g_modUi.pairFrame) ToolRingPair(body, 0.5f * cw);
        if (r.pair != g_modUi.pairFrame || r.body != body) return false;
        float d[2], depth = 0;
        if (!ToolRingDesk(p, d, depth)) return false;   // (behind the desktop camera)
        if (eye == 0) {
            if (r.n == 0) {   // the pair's first marker: centred, with the centroid's smoothed offset
                r.first[0] = d[0];
                r.first[1] = d[1];
                r.anchor[0] = d[0] + (r.deltaValid ? r.delta[0] : 0.0f);
                r.anchor[1] = d[1] + (r.deltaValid ? r.delta[1] : 0.0f);
            }
            ++r.n;
            r.sum[0] += d[0];
            r.sum[1] += d[1];
        } else if (r.n == 0) {
            return false;   // (the left eye had none)
        }
        // View: world -> the mod's desktop camera, inverse(E) = flat x inverse(tool body).
        float* C = (float*)(g_gameObj + g_off3dCam);   // projection, then the view (world -> eye, column-major)
        cam3d = C;
        memcpy(saveCam, C, sizeof(saveCam));
        const Pose view = Mul(r.flat, Inverse(bt));
        const Vec3 X = Rotate(view.q, { 1, 0, 0 }), Y = Rotate(view.q, { 0, 1, 0 }), Z = Rotate(view.q, { 0, 0, 1 });
        const float m[16] = { X.x, X.y, X.z, 0, Y.x, Y.y, Y.z, 0, Z.x, Z.y, Z.z, 0, view.p.x, view.p.y, view.p.z, 1 };
        memcpy(C + 16, m, sizeof(m));
        // Projection (clip.x = m0 x + m8 z, clip.w = -z), so that the binding's way to this eye's
        // canvas (to window pixels - not bent: Set -, then back through the UI camera U: canvas ->
        // pixels = U[16] u + U[28]) gives u = cw/2 + f x / -z - anchor, v = ch/2 - f y / -z - anchor: the
        // same canvas point in both eyes.
        const float u0 = 0.5f * cw - r.anchor[0], v0 = 0.5f * ch - r.anchor[1];
        C[0] = 2.0f * U[16] * r.f / W;
        C[8] = 1.0f - 2.0f * (U[16] * u0 + U[28]) / W;
        C[5] = 2.0f * U[21] * r.f / H;
        C[9] = 2.0f * (U[21] * v0 + U[29]) / H - 1.0f;
        C[1] = C[4] = C[12] = C[13] = 0;
        // Camera controller = the desktop camera: the distance is the mod's own depth.
        const Pose desk = Inverse(view);
        c = (float*)(camObj + g_offCamPos);
        rot = (float*)(camObj + g_offCamRot);
        memcpy(saveC, c, sizeof(saveC));
        memcpy(saveRot, rot, sizeof(saveRot));
        c[0] = desk.p.x;
        c[1] = desk.p.y;
        c[2] = desk.p.z;
        rot[0] = desk.q.x;
        rot[1] = desk.q.y;
        rot[2] = desk.q.z;
        rot[3] = desk.q.w;
        if (r.diagPair == g_modUi.pairFrame) {
            // (compared with what the binding returns: Check)
            check = eye;
            checkUi = ui;
            want[0] = 0.5f * cw + d[0] - r.anchor[0];
            want[1] = 0.5f * ch + d[1] - r.anchor[1];
            want[2] = depth;
            r.bend = bend ? saveBend : -1;
            r.centre[0] = 0.5f * cw;
            r.centre[1] = 0.5f * ch;
            r.sizes[0] = ((const int*)g_gameObj)[0];
            r.sizes[1] = ((const int*)g_gameObj)[1];
            r.sizes[2] = *(const int*)(ui + 0x10);
            r.sizes[3] = *(const int*)(ui + 0x14);
        }
        return true;
    }

    // A marker drawn inside the floating panel's scope of a mod's menu (ModPanel): where the world
    // point is seen from between the eyes THROUGH the panel - the same canvas point in both eyes, on
    // the panel. The 3D camera becomes a camera at the eyes' midpoint whose image plane is the panel
    // (rotation: the panel's, in the world), with a projection that sends a point seen at x/-z, y/-z
    // to u = au + (D x/-z - ox) / k, v = av - (D y/-z - oy) / k (D: distance to the panel's plane,
    // o: the panel's anchor seen from that camera), through this eye's UI camera V (as SetDesktop).
    // The controller is that camera: the distance is from between the eyes.
    bool SetPanel(int eye)
    {
        const ModPanelState& P = g_panel;
        BYTE* camObj = *(BYTE**)(g_gameObj + g_offGamePlayer);
        float* U = (float*)(g_gameObj + g_offUiCam);
        if (P.qFrame[eye] != g_frameCounter || !camObj || !g_offCamPos || !g_offCamRot || U[0] <= 0 || U[5] >= 0 ||
            U[16] <= 0 || U[21] <= 0)
            return false;
        const float W = 2.0f / U[0], H = -2.0f / U[5];
        const float ws = std::max(0.1f, Cfg().worldScale);
        const Vec3 mid = PanelEyeMid();
        const Pose& er = P.eyeRoom[eye];
        const Pose& ew = P.eyeWorld[eye];
        const Quat R = Normalize(ew.q * Conjugate(er.q));   // room -> world (the room's yaw)
        const Vec3 midW = ew.p + Rotate(R, (mid - er.p) * ws);
        const Quat camQ = Normalize(R * P.pose.q);
        const Vec3 o = Rotate(Conjugate(P.pose.q), P.pose.p - mid);
        const float D = -o.z;
        if (!(D > 0.05f)) return false;
        float* C = (float*)(g_gameObj + g_off3dCam);
        cam3d = C;
        memcpy(saveCam, C, sizeof(saveCam));
        const Pose view = Inverse(Pose{ camQ, midW });
        const Vec3 X = Rotate(view.q, { 1, 0, 0 }), Y = Rotate(view.q, { 0, 1, 0 }), Z = Rotate(view.q, { 0, 0, 1 });
        const float m[16] = { X.x, X.y, X.z, 0, Y.x, Y.y, Y.z, 0, Z.x, Z.y, Z.z, 0, view.p.x, view.p.y, view.p.z, 1 };
        memcpy(C + 16, m, sizeof(m));
        const float f = D / P.k;   // canvas units per unit of x/-z
        C[0] = 2.0f * U[16] * f / W;
        C[8] = 1.0f - 2.0f * (U[16] * (P.au - o.x / P.k) + U[28]) / W;
        C[5] = 2.0f * U[21] * f / H;
        C[9] = 2.0f * (U[21] * (P.av + o.y / P.k) + U[29]) / H - 1.0f;
        C[1] = C[4] = C[12] = C[13] = 0;
        c = (float*)(camObj + g_offCamPos);
        rot = (float*)(camObj + g_offCamRot);
        memcpy(saveC, c, sizeof(saveC));
        memcpy(saveRot, rot, sizeof(saveRot));
        c[0] = midW.x;
        c[1] = midW.y;
        c[2] = midW.z;
        rot[0] = camQ.x;
        rot[1] = camQ.y;
        rot[2] = camQ.z;
        rot[3] = camQ.w;
        ++g_worldPixelMoved[1];
        return true;
    }

    void Set(void* ctx, void* args)
    {
        if (!g_off3dCam || !g_luaArgVec || !g_gameObj || !g_vrActive || !ctx || !args) return;
        BYTE* ui = *(BYTE**)((BYTE*)ctx + g_offScriptUi);
        const int eye = g_rightThisFrame ? 1 : 0;
        // A marker on the held mod tool from a window's script: that window is a tool's ring
        // (ToolRing), not a menu (ModPanel: the revolver's ring is centred - on its own panel fixed in
        // the room with modUiToolFixed, PanelToolPlace - or at the centre of the view).
        const bool onTool = g_panel.ready && ui && HeldToolMarker(ctx, args);
        if (onTool) {
            g_panel.toolUi = ui;
            g_panel.toolFrame = g_frameCounter;
            if (g_panel.active && g_panel.ui == ui && g_panel.kind == kPanelMenu) {
                g_panel.kind = kPanelTool;
                if (g_panel.swapped) PanelEnd(nullptr);
                if (!PanelToolFixed()) {
                    if (PanelLog()) Log("Engine: mod panel: its window draws on the held tool - shown as a tool ring instead");
                } else {
                    PanelToolPlace();
                    // ("mod panel opened ... anchor ... k ...; centre ...": the pose mockctl.py pointpanel aims at)
                    if (PanelLog())
                        Log("Engine: mod panel opened again (ui %p: its window draws on the held tool - a tool ring instead, "
                            "fixed in the room straight ahead at eye height; anchor %.0f %.0f; k %.5f; %s)", ui, g_panel.au,
                            g_panel.av, g_panel.k, PanelPoseText(g_panel.pose));
                }
            }
        }
        const bool panel = g_panel.swapped && ui == g_panel.scopeUi;   // (inside the panel's scope)
        const bool flat = ui && ui == g_modUi.ui && ModUiNow(eye) && !PanelSession();
        // (the tool ring fixed in the room: its markers on the held tool as on a desktop screen, centred on the
        // canvas centre = its panel's anchor - also those its script projects before UiMakeInteractive, outside
        // the scope, as the revolver does; its other markers as a menu's: through the panel inside the scope,
        // each eye's own outside it)
        const bool toolPanel = onTool && ui == g_modUi.ui && ModUiNow(eye) && PanelSession() &&
                               g_panel.kind == kPanelTool && g_panel.ui == ui;
        if ((flat || panel || toolPanel) && g_offUiPixelBend) {
            // (no barrel distortion: the same canvas point is at a different window pixel in each eye)
            bend = (int*)(g_gameObj + g_offUiPixelBend);
            saveBend = *bend;
            *bend = 0;
        }
        if (toolPanel && SetDesktop(ui, ctx, args, eye)) return;
        if (panel && SetPanel(eye)) return;
        if (flat && Cfg().modUiCenterTool && SetDesktop(ui, ctx, args, eye)) return;
        float s = 1;
        Vec3 mw;   // the tool's muzzle (world)
        if (g_scaledBody && g_scaledS != 1.0f && g_scaledS > 0.05f && ToolScript(ctx))
            if (BYTE* pl = LocalPlayer())
                if (*(BYTE**)(pl + kPlayerToolBody) == g_scaledBody) {
                    float a[3] = {};
                    g_luaArgVec(args, 0, a, 3);
                    const float* b = (const float*)(g_scaledBody + 0x40);   // tool body: world position, then rotation
                    const Quat bq = Normalize({ b[3], b[4], b[5], b[6] });
                    const Vec3 bp{ b[0], b[1], b[2] };
                    const Vec3 p = Rotate(Conjugate(bq), Vec3{ a[0], a[1], a[2] } - bp);
                    Vec3 lo, hi;
                    const float pad = 0.05f;
                    if (HeldToolBox(g_scaledBody, lo, hi) && p.x > lo.x - pad && p.x < hi.x + pad && p.y > lo.y - pad &&
                        p.y < hi.y + pad && p.z > lo.z - pad && p.z < hi.z + pad) {
                        s = g_scaledS;
                        mw = bp + Rotate(bq, g_scaledPivot);
                    }
                }
        if (s == 1.0f && !flat) return;
        float* P = (float*)(g_gameObj + g_off3dCam);   // projection; the view (world -> eye) follows
        float* V = P + 16;
        auto toEye = [V](Vec3 a) {
            return Vec3{ V[0] * a.x + V[4] * a.y + V[8] * a.z, V[1] * a.x + V[5] * a.y + V[9] * a.z,
                         V[2] * a.x + V[6] * a.y + V[10] * a.z };
        };
        auto toWorld = [V](Vec3 a) {
            return Vec3{ V[0] * a.x + V[1] * a.y + V[2] * a.z, V[4] * a.x + V[5] * a.y + V[6] * a.z,
                         V[8] * a.x + V[9] * a.y + V[10] * a.z };
        };
        Vec3 from = toWorld({ V[12], V[13], V[14] }) * -1.0f;   // this eye
        if (flat) from = from + toWorld(g_eyeOffset.p * g_curK) * (eye ? -0.5f : 0.5f);   // between the eyes
        t = V + 12;
        memcpy(saveT, t, sizeof(saveT));
        const Vec3 nt = toEye(mw * (1.0f - s) - from) * (1.0f / s);
        t[0] = nt.x;
        t[1] = nt.y;
        t[2] = nt.z;
        if (flat) {
            const float ipd = Length(g_eyeOffset.p) / std::max(0.1f, Cfg().worldScale);
            const float shift = (eye ? -0.5f : 0.5f) * ipd / (ModUiDistance() * g_zoom.pair);
            m8 = P + 8;
            saveM8 = *m8;
            *m8 = saveM8 - P[0] * shift;
            ++g_worldPixelMoved[1];
        }
        if (s != 1.0f) {
            ++g_worldPixelMoved[0];
            if (BYTE* camObj = *(BYTE**)(g_gameObj + g_offGamePlayer)) {
                c = (float*)(camObj + g_offCamPos);
                memcpy(saveC, c, sizeof(saveC));
                const Vec3 nc = mw + (Vec3{ c[0], c[1], c[2] } - mw) * (1.0f / s);
                c[0] = nc.x;
                c[1] = nc.y;
                c[2] = nc.z;
            }
        }
    }

    // (debugLua) After the call: the canvas point the binding returned to the mod - its three
    // results on top of the Lua stack (x, y in the script's current UI transform, which it
    // undid: redone here; distance) - against the one wanted, for ToolRing's diagnostics.
    // `pushed`: its count of results before the call.
    void Check(void* ret, int pushed)
    {
        ToolRing::Eye& e = g_toolRing.diag[check];
        float u = want[0], v = want[1], depth = want[2];
        BYTE* L = ret && g_offRetCount && g_offLuaTop && g_offUiTransform ? *(BYTE**)ret : nullptr;
        const BYTE* top = L ? *(BYTE* const*)(L + g_offLuaTop) : nullptr;
        if (top && *(const int*)((const BYTE*)ret + g_offRetCount) - pushed == 3 && *(const int*)(top - 0x28) == 3 &&
            *(const int*)(top - 0x18) == 3 && *(const int*)(top - 0x08) == 3) {
            const double x = *(const double*)(top - 0x30), y = *(const double*)(top - 0x20);
            const float* T = (const float*)(checkUi + g_offUiTransform);   // (column-major; 2D: z and w unused)
            u = float(T[0] * x + T[4] * y + T[12]);
            v = float(T[1] * x + T[5] * y + T[13]);
            depth = float(*(const double*)(top - 0x10));
            e.err = std::max({ e.err, std::abs(u - want[0]), std::abs(v - want[1]) });
            e.depthErr = std::max(e.depthErr, std::abs(depth - want[2]));
        } else {
            ++e.unread;   // (the wanted values are counted instead)
        }
        if (!e.n) {
            e.first[0] = u;
            e.first[1] = v;
            e.depth = depth;
        }
        ++e.n;
        e.sum[0] += u;
        e.sum[1] += v;
        check = -1;
    }

    // (also on the way out of an error raised inside the call: the camera is never left moved)
    void Restore()
    {
        if (cam3d) memcpy(cam3d, saveCam, sizeof(saveCam));
        if (rot) memcpy(rot, saveRot, sizeof(saveRot));
        if (t) memcpy(t, saveT, sizeof(saveT));
        if (m8) *m8 = saveM8;
        if (c) memcpy(c, saveC, sizeof(saveC));
        if (bend) *bend = saveBend;
        t = m8 = c = cam3d = rot = nullptr;
        bend = nullptr;
    }
    ~WorldPixelView() { Restore(); }
};

// --- The floating panel's scope (ModPanel): R+8 swapped while the window's script draws its window ---
uint32_t g_offUiClip = 0;      // UI context -> its clip rect (x0, y0, x1, y1 window pixels, y up; 0 = none) (0x2d8)
uint32_t g_scissorSlot = 0;    // 2D renderer's vtable: set the scissor (R, x0, y0, x1, y1) (0x50)
using PFN_Scissor = void(__fastcall*)(void* r, int x0, int y0, int x1, int y1);

void PanelFail(const char* where)
{
    ModPanelState& P = g_panel;
    if (P.swapped) memcpy(P.rp, P.saved, sizeof(P.saved));   // (the game's UI camera for what follows)
    P.swapped = false;
    P.scopeUi = nullptr;
    if (!P.failed) {
        P.failed = true;
        ++P.failures;
        Log("Engine: mod panel: renderer camera overwritten %s (renderer +%x) - this window is shown as before (in front of "
            "the head)%s", where, g_offRendererProj, P.failures >= 3 ? "; floating panels off until the game restarts" : "");
    }
}

// R+8 still holds the panel's matrix (nothing else wrote the renderer's camera inside the scope).
bool PanelGuard(const char* where)
{
    ModPanelState& P = g_panel;
    if (!P.swapped) return false;
    if (memcmp(P.rp, P.written, sizeof(P.written)) == 0) return true;
    PanelFail(where);
    return false;
}

// The clip rect of a clipped window (UiWindow(w, h, true), UiClipRect: flat window pixels of this
// eye, GL y) mapped onto the panel: the box of its 4 corners through canvas -> Q -> pixels.
void PanelScissor(BYTE* ui)
{
    ModPanelState& P = g_panel;
    if (!P.swapped || ui != P.scopeUi || !g_offUiClip || !g_scissorSlot || !g_gameObj) return;
    const float* r = (const float*)(ui + g_offUiClip);
    if (r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0) return;   // (no clip: the engine turned the scissor off)
    const float* Q = P.Q[P.eye];
    const float* V = P.view;
    const float W = P.qIn[P.eye][8], H = P.qIn[P.eye][9];
    float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
    bool full = false;
    for (int k = 0; k < 4 && !full; ++k) {
        const float px = (k & 1) ? r[2] : r[0], py = H - ((k & 2) ? r[3] : r[1]);
        const float u = (px - V[12]) / V[0], v = (py - V[13]) / V[5];
        const float cx = Q[0] * u + Q[4] * v + Q[12], cy = Q[1] * u + Q[5] * v + Q[13], cw = Q[3] * u + Q[7] * v + Q[15];
        if (!(cw > 1e-4f)) {
            full = true;   // (a corner behind the eye: no clipping)
            break;
        }
        const float X = (cx / cw + 1.0f) * 0.5f * W, Y = (cy / cw + 1.0f) * 0.5f * H;
        lo[0] = std::min(lo[0], X);
        lo[1] = std::min(lo[1], Y);
        hi[0] = std::max(hi[0], X);
        hi[1] = std::max(hi[1], Y);
    }
    int x0 = 0, y0 = 0, x1 = (int)W, y1 = (int)H;
    if (!full) {
        x0 = (int)std::floor(std::clamp(lo[0], 0.0f, W));
        y0 = (int)std::floor(std::clamp(lo[1], 0.0f, H));
        x1 = (int)std::ceil(std::clamp(hi[0], 0.0f, W));
        y1 = (int)std::ceil(std::clamp(hi[1], 0.0f, H));
    }
    BYTE* R = *(BYTE**)(g_gameObj + g_offRenderer);
    if (!R) return;
    PFN_Scissor fn = (*(PFN_Scissor**)R)[g_scissorSlot / 8];
    fn(R, x0, y0, x1, y1);
    if (P.scissors++ < 20)
        Log("Engine: mod panel: clipped window rect %.0f %.0f %.0f %.0f (eye %d pixels) -> scissor %d %d %d %d on the panel",
            r[0], r[1], r[2], r[3], P.eye, x0, y0, x1, y1);
}

// The background blur (UiBackgroundBlur: its amount at ui+0x3d0) of what the window draws on the panel
// is left out. With it, the rect helper 0x4d0330 blurs the rect's FLAT, head-locked spot under a
// scissor it sets itself (renderer vtable+0x50 at 0x4d074b - not through 0x4cf4e0), then puts back
// the flat clip rect (0x4d0999 / 0x4d09c2): a blurred ghost at the old place, and the rest of a
// clipped window cut at its flat place; its callers already pick the blur shader from that amount
// (0x4bfe57, 0x4bffa2, 0x4c8c33...), and 0x4d0b4c binds the blurred image. Set to 0 at the scope's
// start and after each UiBackgroundBlur inside it: plain rects. The UI state is rebuilt at each
// draw (0x4d4a20) and UiPop restores the levels pushed before the scope.
uint32_t g_offUiBlur = 0;   // (0x3d0)
PFN_LuaFn Real_UiBackgroundBlur = nullptr;

void PanelNoBlur(BYTE* ui)
{
    if (!g_offUiBlur || !ui) return;
    float* b = (float*)(ui + g_offUiBlur);
    if (!(*b > 0)) return;
    if (g_panel.blurs++ < 5)
        Log("Engine: mod panel: background blur %.2f of the window left out on the panel (UiBackgroundBlur blurs the flat "
            "image)", *b);
    *b = 0;
}

__int64 __fastcall Hook_UiBackgroundBlur(void* ctx, void* args, void* a3, void* a4)
{
    const __int64 r = Real_UiBackgroundBlur(ctx, args, a3, a4);
    if (g_panel.swapped && ctx && g_offScriptUi) {
        BYTE* ui = *(BYTE**)((BYTE*)ctx + g_offScriptUi);
        if (ui && ui == g_panel.scopeUi) PanelNoBlur(ui);
    }
    return r;
}

// The scope begins (UiMakeInteractive, or the whole draw): R+8 = Q[eye] x inverse(R+0x48). Only in a
// scene script's draw with this frame's UI camera, with this eye's Q built this frame.
bool PanelBegin(BYTE* ui, int depth)
{
    ModPanelState& P = g_panel;
    if (P.swapped || !P.drawMain || !g_offRenderer || !g_gameObj) return false;
    const int eye = g_rightThisFrame ? 1 : 0;
    if (P.qFrame[eye] != g_frameCounter) return false;   // (no VR room camera this frame: drawn as before)
    BYTE* R = *(BYTE**)(g_gameObj + g_offRenderer);
    if (!R) return false;
    float* rp = (float*)(R + g_offRendererProj);
    const float* rv = rp + 16;
    // (the renderer holds this frame's UI camera - not an in-world screen's, not a pass of its own)
    const bool same = memcmp(rp, g_gameObj + g_offUiCam, 32 * sizeof(float)) == 0;
    if (!same || !(rv[0] > 0 && rv[5] > 0) || rv[1] != 0 || rv[2] != 0 || rv[3] != 0 || rv[4] != 0 || rv[6] != 0 ||
        rv[7] != 0 || rv[8] != 0 || rv[9] != 0 || rv[11] != 0 || rv[15] != 1) {
        if (P.refusals++ < 10)
            Log("Engine: mod panel: not drawn on the panel this frame (eye %d) - the 2D renderer holds %s", eye,
                same ? "a camera of an unexpected form" : "another camera than the frame's UI camera");
        return false;
    }
    const float* Q = P.Q[eye];
    float W[16];
    for (int r = 0; r < 4; ++r) {   // Q x inverse(V), V = canvas -> pixels (2D: scale + shift; Q has no z column)
        W[r] = Q[r] / rv[0];
        W[4 + r] = Q[4 + r] / rv[5];
        W[8 + r] = 0;
        W[12 + r] = Q[12 + r] - Q[r] * rv[12] / rv[0] - Q[4 + r] * rv[13] / rv[5];
    }
    // (a window's first stereo pair: its anchor is not known before the left eye has measured its
    // content box - NP's sidebar would flash at the canvas centre's place, 0.8 m to the left of the
    // panel, the very place it was complained about. That pair is drawn nowhere: every vertex at clip
    // (2, 2, 0, 1), outside the view and of no area (the MVP only goes to the shaders' constants);
    // its quads are still tested and measured - the test uses the flat camera, not R+8. At most 3
    // pairs from its first scope: a window made only of buttons gives no box.)
    const bool own = P.active && ui == P.ui;
    if (own && !P.firstBegin) P.firstBegin = g_frameCounter;
    if (own && !P.bbPairOk && g_frameCounter - P.firstBegin < 6) {
        memset(W, 0, sizeof(W));
        W[12] = W[13] = 2.0f;
        W[15] = 1.0f;
        ++P.hiddenDraws;
    } else {
        P.visible = true;
    }
    memcpy(P.saved, rp, sizeof(P.saved));
    memcpy(P.view, rv, sizeof(P.view));
    memcpy(rp, W, sizeof(W));
    memcpy(P.written, W, sizeof(W));
    P.rp = rp;
    P.swapped = true;
    P.scopeUi = ui;
    P.scopeDepth = depth;
    P.eye = eye;
    if (eye == 0) ++P.scopes;
    ++P.drawn;
    PanelScissor(ui);   // (a clip already active: an outer UiWindow(..., true))
    PanelNoBlur(ui);    // (a background blur set before UiMakeInteractive)
    return true;
}

void PanelEnd(const char* why)
{
    ModPanelState& P = g_panel;
    if (!P.swapped) return;
    if (!PanelGuard(why ? why : "during the window's drawing")) return;   // (PanelFail restored it)
    memcpy(P.rp, P.saved, sizeof(P.saved));
    P.swapped = false;
    P.scopeUi = nullptr;
}

// Inside the scope, the engine's on-screen test (flat, per eye) would drop the parts of the window
// that fall outside one eye's image - but on the panel they are seen: one-eyed holes. A quad it
// rejected is drawn unless the script's clip rect rejects it (that part of the test, the same
// in both eyes: the rect and the quad are shifted alike), as the engine does it: the quad's lo and
// hi corners through ui+0x58 x M, against the rect (int truncations included).
bool PanelClipVisible(BYTE* ui, const float* m, const float* lo, const float* hi)
{
    if (!g_offUiClip) return true;
    const float* r = (const float*)(ui + g_offUiClip);
    if (r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0) return true;
    const float* V = g_panel.view;
    auto px = [&](const float* p) { return V[0] * (m[0] * p[0] + m[4] * p[1] + m[12]) + V[12]; };
    auto py = [&](const float* p) { return V[5] * (m[1] * p[0] + m[5] * p[1] + m[13]) + V[13]; };
    const float minx = std::min(px(lo), px(hi)), maxx = std::max(px(lo), px(hi));
    const float miny = std::min(py(lo), py(hi)), maxy = std::max(py(lo), py(hi));
    const float H = (float)*(const int*)(ui + 0x14);   // (the script's height, as the test reads it)
    const int x0 = (int)r[0], hgt = (int)(r[3] - r[1]);
    const int x1 = (int)(r[2] - r[0]) + x0, top = (int)(H - r[1] - (float)hgt);
    return (float)x1 > minx && maxx > (float)x0 && (float)(top + hgt) > miny && maxy > (float)top;
}

// A quad of the window (left eye): its canvas box joins the content box. Quads of 95 % of the canvas
// or more (full-screen dimmers) and absurd ones are left out.
void PanelAddQuad(const float* m, const float* lo, const float* hi)
{
    ModPanelState& P = g_panel;
    float b[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1) ? hi[0] : lo[0], y = (k & 2) ? hi[1] : lo[1];
        const float u = m[0] * x + m[4] * y + m[12], v = m[1] * x + m[5] * y + m[13];
        b[0] = std::min(b[0], u);
        b[1] = std::min(b[1], v);
        b[2] = std::max(b[2], u);
        b[3] = std::max(b[3], v);
    }
    if (!(b[2] - b[0] < 0.95f * P.cw || b[3] - b[1] < 0.95f * P.ch)) return;
    if (!(std::abs(b[0]) < 1e5f && std::abs(b[1]) < 1e5f && std::abs(b[2]) < 1e5f && std::abs(b[3]) < 1e5f)) return;
    if (!P.bbOk) {
        memcpy(P.bb, b, sizeof(b));
        P.bbOk = true;
    } else {
        P.bb[0] = std::min(P.bb[0], b[0]);
        P.bb[1] = std::min(P.bb[1], b[1]);
        P.bb[2] = std::max(P.bb[2], b[2]);
        P.bb[3] = std::max(P.bb[3], b[3]);
    }
}

// The window's script called UiMakeInteractive in a scene script's draw: its session, what kind of
// window it is, and its scope.
void PanelOnInteractive(BYTE* ui, void* ctx)
{
    ModPanelState& P = g_panel;
    const int eye = g_rightThisFrame ? 1 : 0;
    if (!PanelEnabled() || !P.drawMain) return;
    if (P.active && P.ui == ui) P.shown = g_frameCounter;   // (kept, also through a frame drawn as before)
    if (P.active && P.ui != ui) {
        // (another window already shown: this one drawn as before - and, shown 0.5 s along with it,
        // that one too: PanelPairUpdate)
        P.otherUi = ui;
        P.otherFrame = g_frameCounter;
        return;
    }
    if (P.qFrame[eye] != g_frameCounter) return;   // (no VR room camera this frame)
    if (!P.active) PanelSessionStart(ui);
    P.shown = g_frameCounter;
    if (P.kind == kPanelUnknown) {
        const bool marks = P.toolUi == ui && g_frameCounter - P.toolFrame <= 1;
        const bool toolScript = ctx == g_toolScriptCtx && g_frameCounter - g_toolScriptFrame <= 2;
        if (marks) {
            P.kind = kPanelTool;
            if (PanelToolFixed()) {
                PanelToolPlace();
                // ("mod panel opened ... anchor ... k ...; centre ...": the pose mockctl.py pointpanel aims at)
                if (PanelLog())
                    Log("Engine: mod panel opened (ui %p, the window draws on the held tool ('%s'): its ring fixed in the room, "
                        "straight ahead at eye height - yaw %+.0f pitch %+.0f deg, %.2f m; anchor %.0f %.0f of the %.0f x %.0f "
                        "canvas; k %.5f; %s)", ui, g_scriptToolName.c_str(), P.place.yaw, P.place.pitch, P.place.dist, P.au,
                        P.av, P.cw, P.ch, P.k, PanelPoseText(P.pose));
            } else if (PanelLog()) {
                Log("Engine: mod panel: the window draws on the held tool ('%s') - not a floating panel: its ring stays at the "
                    "centre of the view (head-locked canvas)", g_scriptToolName.c_str());
            }
        } else if (!toolScript || P.age >= 0.3f) {   // (the held tool's own window: its ring may come first)
            P.kind = kPanelMenu;
            if (!PanelKindOn(kPanelMenu)) {
                if (PanelLog())
                    Log("Engine: mod panel: a menu (ui %p) - not a floating panel (modUiPanel off): shown as before, in front "
                        "of the head", ui);
            } else if (PanelLog())
                Log("Engine: mod panel opened (ui %p, a menu shown as a floating panel %s: yaw %+.0f pitch %+.0f deg, %.2f m; "
                    "anchor %.0f %.0f of the %.0f x %.0f canvas; k %.5f; %s%s%s)",
                    ui, P.placed ? "at its saved place" : "in front of the gaze", P.place.yaw, P.place.pitch, P.place.dist,
                    P.au, P.av, P.cw, P.ch, P.k, PanelPoseText(P.pose), P.rmbAware ? "; it reads the right mouse button" : "",
                    P.rmbHooks ? "" : "; right-button reads not watched: the grip moves it from its frame band or the other hand");
        }
    }
    if (!PanelKindOn(P.kind) || P.failed) return;
    P.depth0 = UiDepth(ui) == 0;
    if (!P.swapped) PanelBegin(ui, UiDepth(ui));
}

// The whole draw of this script goes to the panel: it draws its window before UiMakeInteractive; or
// its whole draw is the window (UiMakeInteractive at the top level, nothing drawn before it in the
// last pair: Natural Phenomena) - then also the frames right after it closed, before the session
// ends; or the window closed less than 0.4 s ago (its slide-out).
bool PanelWholeDraw(BYTE* ui)
{
    const ModPanelState& P = g_panel;
    if (!ui || !PanelEnabled()) return false;
    if (P.active) return P.ui == ui && PanelKindOn(P.kind) && !P.failed && (P.wholeDraw || (P.depth0 && P.preLast == 0));
    return P.lingerUi == ui && P.lingerT > 0;
}

// Script::draw (0x49eb20): (script, a2, a3, width, height, camera). Scene scripts get the frame's UI
// camera (game+0x2c8); in-world screens their own.
using PFN_ScriptDraw = __int64(__fastcall*)(BYTE* script, void* a2, void* a3, int w, int h, const float* cam);
PFN_ScriptDraw Real_ScriptDraw = nullptr;

__int64 __fastcall Hook_ScriptDraw(BYTE* script, void* a2, void* a3, int w, int h, const float* cam)
{
    ModPanelState& P = g_panel;
    if (P.swapped) PanelEnd(nullptr);   // (never nested; safety)
    const bool prevMain = P.drawMain;
    BYTE* const prevUi = P.drawUi;
    BYTE* ui = script && g_offScriptUi ? *(BYTE**)(script + g_offScriptUi) : nullptr;
    P.drawMain = ui && g_gameObj && cam == (const float*)(g_gameObj + g_offUiCam);
    P.drawUi = ui;
    if (P.drawMain && PanelWholeDraw(ui)) PanelBegin(ui, -1);
    struct End { ~End() { PanelEnd(nullptr); } } end;   // (also if the draw unwinds)
    const __int64 r = Real_ScriptDraw(script, a2, a3, w, h, cam);
    PanelEnd(nullptr);
    P.drawMain = prevMain;
    P.drawUi = prevUi;
    return r;
}

// The clip function of UiWindow(w, h, true) / UiClipRect / background blur (0x4cf4e0: ui, width,
// height, intersect with the current rect): its flat scissor, then the panel's.
using PFN_UiClip = void(__fastcall*)(BYTE* ui, float w, float h, bool intersect);
PFN_UiClip Real_UiClip = nullptr;

void __fastcall Hook_UiClip(BYTE* ui, float w, float h, bool intersect)
{
    Real_UiClip(ui, w, h, intersect);
    if (g_panel.swapped && ui == g_panel.scopeUi) PanelScissor(ui);
}

__int64 __fastcall Hook_UiWorldToPixel(void* ctx, void* args, void* a3, void* a4)
{
    WorldPixelView view;   // (parts of a resized mod tool, interactive windows)
    view.Set(ctx, args);
    const int pushed = view.check >= 0 && a3 && g_offRetCount ? *(const int*)((const BYTE*)a3 + g_offRetCount) : 0;
    __int64 r = Real_UiWorldToPixel(ctx, args, a3, a4);
    view.Restore();
    if (view.check >= 0) view.Check(a3, pushed);
    if (BYTE* ui = ctx ? *(BYTE**)((BYTE*)ctx + g_offScriptUi) : nullptr) {
        int depth = UiDepth(ui);
        if (UiWorldMark* m = WorldMark(ui, false)) m->depth = std::min(m->depth, depth);
        else if (UiWorldMark* n = WorldMark(ui, true)) *n = { ui, depth, g_frameCounter };
    }
    return r;
}

__int64 __fastcall Hook_UiPop(void* ctx, void* args, void* a3, void* a4)
{
    __int64 r = Real_UiPop(ctx, args, a3, a4);
    if (BYTE* ui = ctx ? *(BYTE**)((BYTE*)ctx + g_offScriptUi) : nullptr) {
        if (UiWorldMark* m = WorldMark(ui, false))
            if (UiDepth(ui) < m->depth) m->frame = 0;   // left the marker's UiPush scope
        // (a mod's menu panel: its scope ends when the UiPush level of its UiMakeInteractive is left;
        // otherwise the clip rect UiPop restored, onto the panel)
        if (g_panel.swapped && ui == g_panel.scopeUi) {
            if (g_panel.scopeDepth >= 0 && UiDepth(ui) < g_panel.scopeDepth) PanelEnd("at the end of its UiPush level");
            else PanelScissor(ui);
        }
    }
    return r;
}

__int64 __fastcall Hook_UiMakeInteractive(void* ctx, void* args, void* a3, void* a4)
{
    g_uiInteractiveFrame = g_frameCounter;
    BYTE* ui = nullptr;
    if (ctx && g_offScriptUi) {   // (ModUiPointer)
        ui = *(BYTE**)((BYTE*)ctx + g_offScriptUi);
        g_modUi.ui = ui;
        g_modUi.shownFrame = g_frameCounter;
    }
    const __int64 r = Real_UiMakeInteractive(ctx, args, a3, a4);
    if (ui && g_panel.ready && ui == g_panel.drawUi) PanelOnInteractive(ui, ctx);   // (a menu on a floating panel)
    return r;
}

// Return addresses of the draw tests whose quad never moves (see above).
const void* g_uiFixedCalls[8] = {};
int g_uiFixedCallCount = 0;

bool AimUiQuad(BYTE* ui, const float* m, const float* lo, const float* hi)
{
    if (g_frameCounter - g_uiInteractiveFrame <= 2) return false;   // (also the previous stereo pair)
    const float* P = (const float*)(g_gameObj + g_offUiCam);   // pixels -> clip
    const float* V = P + 16;                                    // canvas -> pixels
    if (P[0] <= 0 || P[5] >= 0 || V[0] <= 0 || V[5] <= 0) return false;
    float sx = std::abs(hi[0] - lo[0]), sy = std::abs(hi[1] - lo[1]);
    float ex = std::abs(m[0]) * sx + std::abs(m[4]) * sy;   // size on the canvas
    float ey = std::abs(m[1]) * sx + std::abs(m[5]) * sy;
    if (std::max(ex, ey) > kAimUiMaxSize) return false;
    // Quad centre relative to the canvas centre (UiCenter, UiMiddle = half the screen in canvas units).
    float lx = 0.5f * (lo[0] + hi[0]), ly = 0.5f * (lo[1] + hi[1]);
    float x = m[0] * lx + m[4] * ly + m[12] - 1.0f / (P[0] * V[0]);
    float y = m[1] * lx + m[5] * ly + m[13] + 1.0f / (P[5] * V[5]);
    if (x * x + y * y > kAimUiZone * kAimUiZone) return false;
    if (UiWorldMark* w = WorldMark(ui, false))
        if (UiDepth(ui) >= w->depth) return false;   // a world marker (UiWorldToPixel)
    return true;
}

bool __fastcall Hook_UiRectVisible(BYTE* ui, float* m, const float* lo, const float* hi)
{
    ModPanelState& P = g_panel;
    if (P.swapped && ui == P.scopeUi && m && lo && hi && PanelGuard("during the window's drawing")) {
        // A mod's menu on its floating panel: the flat per-eye screen test would leave one-eyed holes
        // (PanelClipVisible); its quads make the content box (left eye).
        bool r = Real_UiRectVisible(ui, m, lo, hi);
        if (!r) r = PanelClipVisible(ui, m, lo, hi);
        if (r && P.eye == 0) {
            PanelAddQuad(m, lo, hi);
            ++P.quads;
            ++P.quadsTotal;
        }
        return r;
    }
    if (P.active && !P.swapped && ui && ui == P.ui && P.drawMain && P.drawUi == ui && !g_rightThisFrame) ++P.preQuads;
    if (g_aimUiFrame && g_aimUiFrame == g_frameCounter && ui && m && lo && hi) {
        const void* ret = _ReturnAddress();   // (here, not in a helper: the caller of the test)
        bool fixed = false;
        for (int i = 0; i < g_uiFixedCallCount; ++i) fixed = fixed || g_uiFixedCalls[i] == ret;
        if (!fixed && AimUiQuad(ui, m, lo, hi)) {
            m[12] += g_aimUiOff[0];
            m[13] += g_aimUiOff[1];
            ++g_aimUiMoved;
        }
    }
    return Real_UiRectVisible(ui, m, lo, hi);
}

// A Ui* binding's implementation. Unlike the other bindings (FindLuaBinding), the UI
// functions are registered from a static table of { implementation, name, next }
// entries: found by the pointer to their name.
BYTE* FindUiBinding(const char* fname)
{
    const size_t len = strlen(fname) + 1;
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    BYTE* name = nullptr;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !name; ++i, ++sec) {
        if (memcmp(sec->Name, ".rdata", 7) != 0) continue;
        BYTE* s = g_base + sec->VirtualAddress;
        for (size_t k = 1; k + len < sec->Misc.VirtualSize; ++k)
            if (s[k - 1] == 0 && memcmp(s + k, fname, len) == 0) { name = s + k; break; }
    }
    if (!name) return nullptr;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".data", 6) != 0 && memcmp(sec->Name, ".rdata", 7) != 0) continue;
        BYTE* s = g_base + sec->VirtualAddress;
        for (size_t k = 8; k + 8 <= sec->Misc.VirtualSize; k += 8) {
            if (*(BYTE**)(s + k) != name) continue;
            BYTE* fn = *(BYTE**)(s + k - 8);
            if (fn >= g_text && fn < g_text + g_textSize) return fn;
        }
    }
    return nullptr;
}

// Records the calls to the draw test `vis` inside a Ui* binding (the whole function,
// bounded by its unwind data) as calls whose quad never moves.
void AddFixedUiCalls(const char* binding, BYTE* vis)
{
    BYTE* fn = FindUiBinding(binding);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = fn ? RtlLookupFunctionEntry((DWORD64)fn, &imageBase, nullptr) : nullptr;
    if (!rf || (BYTE*)imageBase + rf->BeginAddress != fn) return;
    BYTE* end = (BYTE*)imageBase + rf->EndAddress;
    for (BYTE* q = fn; q + 5 <= end && g_uiFixedCallCount < 8; ++q)
        if (q[0] == 0xE8 && RelTarget(q + 5, q + 1) == vis) g_uiFixedCalls[g_uiFixedCallCount++] = q + 5;
}

void InitCrosshairAim()
{
    // The "UI rect on screen" test (Hook_UiRectVisible) starts with "an empty rect is not":
    //   mov [rsp+8],rbx ; ... ; sub rsp,X ; movss xmm0,[r8] ; mov rbp,r9 ; ucomiss xmm0,[r9] ;
    //   mov r14,r8 ; mov rbx,rcx ; jp ; jne ; movss xmm0,[r8+4] ; ucomiss xmm0,[r9+4]
    BYTE* vis = Scan("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC ?? ?? 00 00 "
                     "F3 41 0F 10 00 49 8B E9 41 0F 2E 01 4D 8B F0 48 8B D9 7A ?? 75 ?? F3 41 0F 10 40 04 41 0F 2E 41 04");
    BYTE* w2p = FindUiBinding("UiWorldToPixel");
    BYTE* pop = FindUiBinding("UiPop");
    BYTE* inter = FindUiBinding("UiMakeInteractive");
    // Each binding starts with "mov rbx,[rcx+ui]" (script -> UI context); UiPop then
    // checks the UiPush depth: mov eax,[rbx+depth] ; test eax,eax ; jg
    BYTE* a = w2p ? ScanIn(w2p, 0x30, "48 8B 99 ?? ?? 00 00") : nullptr;
    BYTE* b = pop ? ScanIn(pop, 0x30, "48 8B 99 ?? ?? 00 00") : nullptr;
    BYTE* c = inter ? ScanIn(inter, 0x30, "48 8B 99 ?? ?? 00 00") : nullptr;
    BYTE* d = pop ? ScanIn(pop, 0x100, "8B 83 ?? ?? 00 00 85 C0 7F") : nullptr;
    if (!g_uiReady || !vis || !a || !b || !c || !d || U32(a + 3) != U32(b + 3) || U32(c + 3) != U32(b + 3)) {
        Log("Engine: UI draw test not recognised (%p %p %p %p) - mods' crosshairs stay in the centre of the view",
            vis, w2p, pop, inter);
        return;
    }
    g_offScriptUi = U32(b + 3);
    g_offUiDepth = U32(d + 2);
    // (before the draw test is hooked; Teardown 2.0.1: one call in each)
    AddFixedUiCalls("UiImageAlpha", vis);
    AddFixedUiCalls("UiImageOverlayAlpha", vis);
    AddFixedUiCalls("UiTextInput", vis);
    // (the marker hooks first: the draws are only moved once world markers are recognised)
    if (!HookFunction(w2p, (void*)Hook_UiWorldToPixel, (void**)&Real_UiWorldToPixel) ||
        !HookFunction(pop, (void*)Hook_UiPop, (void**)&Real_UiPop) ||
        !HookFunction(inter, (void*)Hook_UiMakeInteractive, (void**)&Real_UiMakeInteractive) ||
        !HookFunction(vis, (void*)Hook_UiRectVisible, (void**)&Real_UiRectVisible))
        return;
    Log("Engine: mods' 2D crosshairs follow the controller aim (UI draw test +%llx, script ui +%x, depth +%x, "
        "%d calls never moved)", (unsigned long long)(vis - g_base), g_offScriptUi, g_offUiDepth, g_uiFixedCallCount);
}

// The window pixel the Ui* functions read the mouse at (MousePixel, Teardown 2.0.1 +4f27a0:
// the script's input copy, mouse position + offset, as floats): in VR, the cursor of a
// mod's interactive window in this eye (ModUiPointer). Only its calls from the Ui*
// functions (UiGetMousePos, UiIsMouseInRect, buttons, sliders, text...): the editor and
// the in-world screens keep the real mouse.
using PFN_MousePixel = float*(__fastcall*)(BYTE* input, float* out);
PFN_MousePixel Real_MousePixel = nullptr;
const void* g_mouseUiCalls[16] = {};
int g_mouseUiCallCount = 0;

float* __fastcall Hook_MousePixel(BYTE* input, float* out)
{
    const void* ret = _ReturnAddress();   // (here, not in a helper: the Ui* function's call)
    float* r = Real_MousePixel(input, out);
    if (g_modUi.mouseFrame == g_frameCounter && r)
        for (int i = 0; i < g_mouseUiCallCount; ++i)
            if (g_mouseUiCalls[i] == ret) {
                r[0] = g_modUi.px;
                r[1] = g_modUi.py;
                ++g_modUi.reads;
                if (g_panel.swapped && !g_rightThisFrame) ++g_panel.mouse;   // (a button of the panel's window)
                break;
            }
    return r;
}

void InitModUiPointer()
{
    if (!g_uiReady || !g_offScriptUi || !Real_UiMakeInteractive || !Real_UiWorldToPixel) {
        Log("Engine: mods' interactive windows: UI hooks missing - not usable with the controllers");
        return;
    }
    BYTE* w2p = FindUiBinding("UiWorldToPixel");
    BYTE* mouse = FindUiBinding("UiGetMousePos");
    // UiWorldToPixel: reads its point (mov r9d,3 ; ... ; call ArgVec), projects it with the 3D
    // camera (mov r9,[g_game] ; ... ; lea rcx,[r9+cam]), then measures its distance from the camera
    // controller's position (mov rcx,[rax+player] ; movss xmm9,[rcx+pos]).
    BYTE* a = w2p ? ScanIn(w2p, 0x80, "41 B9 03 00 00 00") : nullptr;
    BYTE* argVec = nullptr;
    for (BYTE* q = a; q && q < a + 0x40 && !argVec; ++q)
        if (q[0] == 0xE8) argVec = RelTarget(q + 5, q + 1);
    BYTE* c = w2p ? ScanIn(w2p, 0xC0, "49 8D 89 ?? ?? 00 00") : nullptr;
    BYTE* d = w2p ? ScanIn(w2p, 0x140, "48 8B 48 ?? F3 44 0F 10 89 ?? ?? 00 00") : nullptr;
    // ArgVec(args, index, out, count): mov [rsp+18h],rbx ; push rdi ; push r14 ; push r15 ; sub rsp,20h ;
    // mov r14,r8 ; movsxd rdi,r9d ; mov rbx,rcx ; cmp edx,[rcx+count] ; jge
    const bool world = argVec && ScanIn(argVec, 64, "48 89 5C 24 ?? 57 41 56 41 57 48 83 EC 20 4D 8B F0 49 63 F9 48 8B D9 3B 51 ?? 0F 8D") == argVec &&
                       c && d && d[3] == g_offGamePlayer && U32(d + 9) == g_offCamPos &&
                       U32(c + 3) + 0x88 == g_offUiCam;   // (the UI camera follows the 3D one)
    if (world) {
        g_off3dCam = U32(c + 3);
        g_luaArgVec = (PFN_ArgVec)argVec;
        // The pixel bend between projection and UI camera (mov rcx,[g_game] ; lea r8,[rbp+X] ;
        // lea rdx,[rbp+Y] ; call bend): after the map checks (cmp dword [rax],7 ; jne ; movss
        // xmm0,[rax+blend] ; jmp ; cmp dword [rax+4],7), cmp dword [rcx+flag],0.
        BYTE* b = ScanIn(c, 0x80, "48 8B 0D ?? ?? ?? ?? 4C 8D 45 ?? 48 8D 55 ?? E8");
        BYTE* fn = b ? RelTarget(b + 20, b + 16) : nullptr;
        BYTE* map = fn && fn >= g_text && fn + 0x100 <= g_text + g_textSize
                        ? ScanIn(fn, 0x60, "83 38 07 75 ?? F3 0F 10 80 ?? ?? 00 00 EB ?? 83 78 04 07")
                        : nullptr;
        BYTE* flag = map ? ScanIn(map, 0x40, "83 B9 ?? ?? 00 00 00") : nullptr;
        if (flag && U32(flag + 2) < 0x10000) g_offUiPixelBend = U32(flag + 2);
        else Log("Engine: UiWorldToPixel's pixel bend not recognised - with the game's barrel distortion on, the "
                 "markers of mods' interactive windows may differ between the eyes");
        // (debugLua) Its results: a push helper (float to double, lua_pushnumber, result count + 1), lua_pushnumber
        // itself (stores the number at the Lua stack's top and moves the top by 10h), and the UI transform it undoes
        // last (patterns below).
        BYTE* p = ScanIn(w2p, 0x300, "48 8B CF E8");
        BYTE* push = p ? RelTarget(p + 8, p + 4) : nullptr;
        BYTE* num = push && push >= g_text && push + 0x20 <= g_text + g_textSize &&
                            ScanIn(push, 0x20, "40 53 48 83 EC 20 48 8B D9 48 8B 09 F3 0F 5A C9 E8 ?? ?? ?? ?? FF 43 ??") == push
                        ? RelTarget(push + 21, push + 17)
                        : nullptr;
        BYTE* tr = ScanIn(w2p, 0x300, "0F 10 83 ?? ?? 00 00 48 8D 4D ?? 0F 29 45");
        if (num && num >= g_text && num + 0x20 <= g_text + g_textSize && tr &&
            ScanIn(num, 0x20, "48 8B 41 ?? F2 0F 11 08 C7 40 08 03 00 00 00 48 83 41 ?? 10") == num && num[3] == num[18]) {
            g_offRetCount = push[23];
            g_offLuaTop = num[3];
            g_offUiTransform = U32(tr + 3);
        }
    }
    // UiGetMousePos: lea rcx,[script+input] ; lea rdx,[rsp+X] ; ... ; call MousePixel
    BYTE* e = mouse ? ScanIn(mouse, 0x120, "48 8D 8E ?? ?? 00 00 48 8D 94 24") : nullptr;
    BYTE* pix = nullptr;
    for (BYTE* q = e; q && q < e + 0x60 && !pix; ++q)
        if (q[0] == 0xE8) pix = RelTarget(q + 5, q + 1);
    // MousePixel: mov eax,[rcx+x] ; add eax,[rcx+dx] ; movd xmm0,eax ; mov eax,[rcx+y] ; add eax,[rcx+dy] ;
    // cvtdq2ps xmm0,xmm0 ; movss [rdx],xmm0
    if (pix && ScanIn(pix, 64, "8B 81 ?? ?? 00 00 03 81 ?? ?? 00 00 66 0F 6E C0 8B 81 ?? ?? 00 00 03 81 ?? ?? 00 00 0F 5B C0 F3 0F 11 02") != pix)
        pix = nullptr;
    // Its calls from the Ui* functions: the block of their code (alphabetical, helpers included).
    BYTE* lo = nullptr;
    BYTE* hi = nullptr;
    for (const char* n : { "UiAlign", "UiBlankButton", "UiGetMousePos", "UiWorldToPixel" })
        if (BYTE* f = FindUiBinding(n)) {
            if (!lo || f < lo) lo = f;
            if (!hi || f > hi) hi = f;
        }
    if (pix && lo)
        for (BYTE* q = lo; q + 5 <= hi + 0x1000 && g_mouseUiCallCount < 16; ++q)
            if (q[0] == 0xE8 && RelTarget(q + 5, q + 1) == pix) g_mouseUiCalls[g_mouseUiCallCount++] = q + 5;
    if (!world || !pix || !g_mouseUiCallCount) {
        Log("Engine: mods' interactive windows not recognised (%p %p %p %p, %d mouse calls)%s", argVec, c, d, pix,
            g_mouseUiCallCount, world ? " - only the markers of resized mod tools are fixed" : "");
        return;
    }
    if (!HookFunction(pix, (void*)Hook_MousePixel, (void**)&Real_MousePixel)) return;
    g_modUi.ready = true;
    Log("Engine: mods' interactive windows used with the controllers (mouse +%llx, %d calls from Ui* functions; "
        "UiWorldToPixel: 3D camera game+%x, ArgVec +%llx, pixel bend flag game+%x, results %s)",
        (unsigned long long)(pix - g_base), g_mouseUiCallCount, g_off3dCam, (unsigned long long)(argVec - g_base),
        g_offUiPixelBend, g_offRetCount ? "readable" : "not readable");
}

void InitCameraHook()
{
    // Game::lateUpdate: mov rdx,[rsi+60h] ; lea r8,[rdx+1DCh] ; add rdx,1D0h ; lea rcx,[rbp-70h] ; call Camera::setTransform
    BYTE* p = Scan("48 8B 56 ?? 4C 8D 82 ?? ?? 00 00 48 81 C2 ?? ?? 00 00 48 8D 4D ?? E8 ?? ?? ?? ??");
    if (!p) {
        Log("Engine: render camera not recognised - the right eye uses the game's own camera");
        return;
    }
    g_offGamePlayer = p[3];
    g_offCamRot = U32(p + 7);
    g_offCamPos = U32(p + 14);
    BYTE* fn = RelTarget(p + 27, p + 23);
    if (g_offCamRot != g_offCamPos + 12) {
        Log("Engine: unexpected render camera layout (pos %x rot %x)", g_offCamPos, g_offCamRot);
        return;
    }
    if (!HookFunction(fn, (void*)Hook_CamSet, (void**)&Real_CamSet)) return;
    Log("Engine: render camera hooked (+%llx, player %x, pos %x, rot %x)", (unsigned long long)(fn - g_base),
        g_offGamePlayer, g_offCamPos, g_offCamRot);
}

void InitScriptCamera()
{
    // Camera update (Teardown 2.0.1 ~RVA 0x36d00), SetCameraTransform request:
    //   cmp byte [rsi+requested],0 ; je ; lea rcx,[rsi+saved] ; mov rdi,rsi ; mov ebx,camPos ; cmp eax,<script mode>
    // (the binding sets "requested"; this switches the camera to the script mode for the frame)
    BYTE* p = Scan("80 BE ?? ?? 00 00 00 0F 84 ?? ?? ?? ?? 48 8D 8E ?? ?? 00 00 48 8B FE BB ?? ?? 00 00 83 F8 ??");
    if (!p || !g_offCamPos || U32(p + 24) != g_offCamPos) {
        Log("Engine: script camera mode not recognised - the stick never acts as the mouse automatically");
        return;
    }
    g_scriptCamMode = p[30];
    Log("Engine: script camera mode %d (request flag +%x)", g_scriptCamMode, U32(p + 2));
}

// Implementation of a Lua binding, found by its name: the registration code passes
// both (lea r8,[impl] ; lea rdx,[name]).
BYTE* FindLuaBinding(const char* fname)
{
    const size_t len = strlen(fname) + 1;
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE* name = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !name; ++i, ++sec) {
        if (strncmp((const char*)sec->Name, ".rdata", 6) != 0) continue;
        BYTE* s = g_base + sec->VirtualAddress;
        for (size_t k = 1; k + len < sec->Misc.VirtualSize; ++k)
            if (memcmp(s + k, fname, len) == 0 && s[k - 1] == 0) { name = s + k; break; }
    }
    if (!name) return nullptr;
    for (size_t i = 7; i + 7 < g_textSize; ++i) {
        BYTE* p = g_text + i;
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15 && RelTarget(p + 7, p + 3) == name && p[-7] == 0x4C &&
            p[-6] == 0x8D && p[-5] == 0x05)
            return RelTarget(p, p - 4);
    }
    return nullptr;
}

// GetShapeLocalTransform / GetShapeWorldTransform (see ShapeAsMade). Both first read
// their shape handle: xor r8d,r8d ; xor edx,edx ; [mov rcx,rax ;] call ArgInt
//   ArgInt (+631b80): cmp edx,[rcx+8] ; jge default ; mov rcx,[rcx] ; inc edx ; jmp lua_tointeger
void InitShapeReadHooks()
{
    BYTE* loc = FindLuaBinding("GetShapeLocalTransform");
    BYTE* world = FindLuaBinding("GetShapeWorldTransform");
    BYTE* a = loc ? ScanIn(loc, 0x40, "45 33 C0 33 D2 E8") : nullptr;
    BYTE* b = world ? ScanIn(world, 0x50, "45 33 C0 33 D2 48 8B C8 E8") : nullptr;
    BYTE* argInt = a ? RelTarget(a + 10, a + 6) : nullptr;
    if (!argInt || !b || RelTarget(b + 13, b + 9) != argInt ||
        ScanIn(argInt, 11, "3B 51 ?? 7D ?? 48 8B 09 FF C2 E9") != argInt) {
        Log("Engine: GetShapeLocalTransform / GetShapeWorldTransform not recognised (%p %p) - mods see the parts of "
            "resized tools as drawn", loc, world);
        return;
    }
    g_luaArgInt = (PFN_ArgInt)argInt;
    g_offArgCount = argInt[2];
    bool l = HookFunction(loc, (void*)Hook_GetShapeLocal, (void**)&Real_GetShapeLocal);
    bool w = l && HookFunction(world, (void*)Hook_GetShapeWorld, (void**)&Real_GetShapeWorld);
    if (!l) g_luaArgInt = nullptr;   // (nothing hooked)
    Log("Engine: GetShapeLocalTransform (+%llx) / GetShapeWorldTransform (+%llx) %s - mods see the parts of resized "
        "tools where they put them", (unsigned long long)(loc - g_base), (unsigned long long)(world - g_base),
        w ? "hooked" : l ? "only the first hooked" : "NOT hooked");
}

const BYTE kToolAnimPrologue[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0xF3, 0x0F, 0x11, 0x48, 0x10 };

// The engine function the SetShapeLocalTransform binding calls once it has found the
// shape from its handle (2.0.x +496550):
//   binding: lea r8,[rsp+30h] ; mov rdx,rbx (shape) ; mov rcx,rsi ; call impl
//   impl:    test rdx,rdx ; je ... ; ... movups [rsi+44h],xmm0 (shape local transform)
// Hooked there (Hook_ShapeLocalImpl); false when not recognised.
bool HookShapeLocalImpl(BYTE* binding)
{
    BYTE* call = ScanIn(binding, 0x140, "4C 8D 44 24 ?? 48 8B D3 48 8B CE E8");
    BYTE* impl = call ? RelTarget(call + 16, call + 12) : nullptr;
    if (!impl || impl < g_text || impl + 0x100 > g_text + g_textSize || ScanIn(impl, 9, "48 85 D2 0F 84") != impl ||
        !ScanIn(impl, 0x100, "0F 11 46 44") || !HookFunction(impl, (void*)Hook_ShapeLocalImpl, (void**)&Real_ShapeLocalImpl)) {
        Log("Engine: SetShapeLocalTransform's engine function not recognised - its binding is hooked instead "
            "(animated parts follow on mod tools of 128 shapes at most)");
        return false;
    }
    Log("Engine: SetShapeLocalTransform hooked in its engine function (+%llx) - animated parts of resized mod tools "
        "follow, one shape at a time", (unsigned long long)(impl - g_base));
    return true;
}

// Lua binding of SetToolTransform, found by its body: the transform copied into the
// player's override field, then the sway amount stored after it
//   movups xmm0,[rsp+30h] ; movups [rax+0A0Ch],xmm0 ; movsd xmm1,[rsp+40h] ; movsd [rax+0A1Ch],xmm1 ;
//   mov ecx,[rsp+48h] ; mov [rax+0A24h],ecx ; movss [rax+0A34h],xmm6
// (SetToolTransformOverride has the same copy followed by "mov byte [rax+0A38h],1").
void InitScriptToolHook()
{
    const char* pat = "0F 10 44 24 ?? 0F 11 80 ?? ?? 00 00 F2 0F 10 4C 24 ?? F2 0F 11 88 ?? ?? 00 00 "
                      "8B 4C 24 ?? 89 88 ?? ?? 00 00 F3 0F 11 B0 ?? ?? 00 00";
    BYTE* start = g_text;
    size_t left = g_textSize;
    while (BYTE* m = ScanIn(start, left, pat)) {
        start = m + 1;
        left = g_textSize - (start - g_text);
        if (U32(m + 8) != g_offToolOverride) continue;
        // Start of the function: 16-byte aligned, after int3 padding.
        BYTE* fn = nullptr;
        for (BYTE* p = m; p > m - 0x200 && p > g_text + 2; --p)
            if (((uintptr_t)p & 15) == 0 && p[-1] == 0xCC && (p[-2] == 0xCC || p[-2] == 0xC3)) { fn = p; break; }
        if (!fn) continue;
        if (!HookFunction(fn, (void*)Hook_SetToolTransform, (void**)&Real_SetToolTransform)) break;
        Log("Engine: SetToolTransform hooked (+%llx) - tools added by mods are held with their own pose "
            "(mod hand pose %x/%x/%x)", (unsigned long long)(fn - g_base), g_offToolHandSet, g_offToolHandR,
            g_offToolHandRUsed);
        if (BYTE* set = FindLuaBinding("SetShapeLocalTransform"); set && !HookShapeLocalImpl(set))
            if (HookFunction(set, (void*)Hook_SetShapeLocal, (void**)&Real_SetShapeLocal))
                Log("Engine: SetShapeLocalTransform hooked (+%llx) - animated parts of resized mod tools follow",
                    (unsigned long long)(set - g_base));
        return;
    }
    Real_SetToolTransform = nullptr;
    Log("Engine: SetToolTransform not recognised - tools added by mods are simply held in the hand");
}

void InitToolHook()
{
    // Override branch of ToolAnim::update:
    //   cmp byte [rcx+0A38h],0 ; jne copy  ...  copy: movups xmm0,[rcx+0A0Ch] ; movups [rcx+844h],xmm0 ; ...
    const char* copyPattern = "0F 10 81 ?? ?? 00 00 0F 11 81 ?? ?? 00 00 F2 0F 10 89 ?? ?? 00 00 F2 0F 11 89 ?? ?? 00 00 "
                              "8B 81 ?? ?? 00 00 89 81 ?? ?? 00 00";
    BYTE* start = g_text;
    size_t left = g_textSize;
    while (BYTE* copy = ScanIn(start, left, copyPattern)) {
        start = copy + 1;
        left = g_textSize - (start - g_text);
        // A "cmp byte [rcx+X],0 ; jne copy" must precede it in the same function...
        BYTE* flag = nullptr;
        for (BYTE* p = copy - 13; p > copy - 0x3000 && p > g_text; --p)
            if (p[0] == 0x80 && p[1] == 0xB9 && p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0x0F && p[8] == 0x85 &&
                RelTarget(p + 13, p + 9) == copy) { flag = p; break; }
        if (!flag) continue;
        // ...whose prologue (after int3 padding or the previous ret) is "mov rax,rsp ; mov [rax+18h],rbx ; movss [rax+10h],xmm1"
        BYTE* fn = nullptr;
        for (BYTE* p = flag; p > flag - 0x3000 && p > g_text; --p)
            if ((p[-1] == 0xCC || p[-1] == 0xC3) && memcmp(p, kToolAnimPrologue, sizeof(kToolAnimPrologue)) == 0) { fn = p; break; }
        if (!fn) continue;
        // mov rsi,rcx ; mov rcx,[rcx+58h]   (the player object)
        BYTE* pl = nullptr;
        for (BYTE* p = fn; p < fn + 0x100; ++p)
            if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xF1 && p[3] == 0x48 && p[4] == 0x8B && p[5] == 0x49) { pl = p; break; }
        if (!pl) continue;
        g_offToolOverride = U32(copy + 3);
        g_offToolFinal = U32(copy + 10);
        g_offToolFlag = U32(flag + 2);
        g_offToolPlayer = pl[6];
        // Built-in tools: "lea rdi,[rsi+X] ... movups xmm0,[rdi] ; movups [rcx+final],xmm0"
        // copies the animated pose (before the desktop offsets) to the final transform.
        for (BYTE* p = fn; p < fn + 0x1800 && !g_offToolAnimPose; ++p)
            if (p[0] == 0x0F && p[1] == 0x10 && p[2] == 0x07 && p[3] == 0x0F && p[4] == 0x11 && p[5] == 0x81 &&
                U32(p + 6) == g_offToolFinal)
                for (BYTE* q = p - 4; q > p - 0x30; --q)
                    if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x7E) { g_offToolAnimPose = q[3]; break; }
        // The call right after "mov rcx,[rcx+58h]" returns the tool id from its name:
        // mov rdi,[rcx+toolAnim] ; mov rdx,[rdi+entry] ; test rdx,rdx ; je ; add rdx,name
        if (pl[7] == 0xE8) {
            BYTE* id = RelTarget(pl + 12, pl + 8);
            if (BYTE* m = ScanIn(id, 0x60, "48 8B B9 ?? ?? 00 00 48 8B 57 ?? 48 85 D2 74 ?? 48 83 C2 ??")) {
                g_offToolEntry = m[10];
                g_offToolName = m[19];
            }
        }
        // Script tools, hand pose set by SetToolHandPoseLocalTransform:
        //   cmp byte [rcx+0A39h],0 ; je ; movups xmm0,[rcx+0A70h] ... cmp byte [rcx+0A8Ch],0 ; je
        if (BYTE* m = ScanIn(fn, 0x1800, "80 B9 ?? ?? 00 00 00 74 ?? 0F 10 81 ?? ?? 00 00 0F 11 45 ?? F2 0F 10 89 ?? ?? 00 00 "
                                         "F2 0F 11 4D ?? 8B 81 ?? ?? 00 00 89 45 ?? 80 B9 ?? ?? 00 00 00 74")) {
            if (U32(m + 2) == g_offToolFlag + 1) {
                g_offToolHandSet = U32(m + 2);
                g_offToolHandR = U32(m + 12);
                g_offToolHandRUsed = U32(m + 44);
            }
        }
        if (!HookFunction(fn, (void*)Hook_ToolAnim, (void**)&Real_ToolAnim)) return;
        Log("Engine: first-person tool placement hooked (+%llx, player %x, override %x -> %x, flag %x, anim %x, "
            "name %x/%x)", (unsigned long long)(fn - g_base), g_offToolPlayer, g_offToolOverride, g_offToolFinal,
            g_offToolFlag, g_offToolAnimPose, g_offToolEntry, g_offToolName);
        InitScriptToolHook();
        if (Real_SetToolTransform) InitShapeReadHooks();   // (mod tools are resized only then)
        return;
    }
    Log("Engine: tool placement not recognised - the tool keeps its normal position");
}

} // namespace

// ---------------------------------------------------------------------------
// Per-eye temporal history
// ---------------------------------------------------------------------------
// Teardown's renderer is temporal: TAA, the disocclusion mask, linear depth and
// the light / reflection / fog denoisers blend each frame with the previous one,
// reprojected with the previous frame's camera. With one eye per engine frame,
// "the previous frame" was always the OTHER eye, 6.4 cm away: every frame was
// resampled at a parallax offset (blur), lighting and reflections were imported
// from the other eye and edges got alternating halos.
// Renderer::render reads all of this state from a few fields, so each eye gets
// its own set, swapped in at render entry: history buffers (10 FBOs), the TAA
// textures (5), the ping-pong indices, the previous camera matrices and the TAA
// jitter counter. The second set is created with the engine's own allocators.
// Auto-exposure stays shared on purpose (both eyes equally bright).
// Found and verified by reverse engineering (docs/REVERSE_ENGINEERING.md).
using PFN_Render = void(__fastcall*)(BYTE* renderer, void* a2, void* a3, void* a4);
using PFN_Release = void(__fastcall*)(void* object);
using PFN_TaaCreate = void(__fastcall*)(BYTE* taa, int w, int h);
PFN_Render Real_Render = nullptr;
PFN_Release g_releaseFbo = nullptr;
PFN_Release g_releaseTex = nullptr;
PFN_TaaCreate g_taaCreate = nullptr;
bool g_historyOk = false;
BYTE* g_renderFn = nullptr;   // Renderer::render (InitHistoryHook), for InitSameNoise's checks

constexpr uint32_t kHistFbo[10] = { 0xa50, 0xa58, 0xaa8, 0xab0, 0xac0, 0xac8, 0xae0, 0xae8, 0xaf8, 0xb00 };
constexpr uint32_t kTaaTex[5] = { 0x50, 0x58, 0x60, 0x68, 0x70 };
constexpr uint32_t kRendTaa = 0xa30, kRendBackend = 0x9c0, kRendIndex = 0x2210, kRendStableVp = 0x22ec,
                   kRendView = 0x2258, kRendReset = 0x23f1;
constexpr size_t kFboDesc = 0x38;   // an FBO object starts with a copy of its creation descriptor

struct EyeHistory {
    void* fbo[10] = {};
    void* tex[5] = {};
    uint32_t index[2] = {};
    float stableVp[16] = {};
    float view[16] = {};
    int taaCounter = 0;
    bool valid = false;
};
EyeHistory g_hist[2];
int g_histActive = 0;
bool g_histPending[2] = {};
uint64_t g_renders = 0, g_lastRender[2] = {};
// [Affichage] stereoParticleMotion: the last frame B (right eye) really rendered with the right eye's own TAA history
// (perEyeHistory: its buffers applied, g_histActive == 1 after Hook_Render's swap). g_hist[1].valid is not enough: it
// stays true once the buffers were created, while a later HistEnsure failure (an FBO missing, a creation failed, no
// TAA / backend) leaves frame B on frame A's history - the previous frame is then frame A's, and zero particle motion
// is the right answer. Set by Hook_Render in each frame B; read by ParticleMotionDecide in the next one (before its
// render: the mesh tasks run before it), so a failure is seen one pair late.
bool g_histRightOwn = false;

void* BackendCreateFbo(void* backend, const void* desc, const char* name)
{
    using Fn = void*(__fastcall*)(void*, const void*, const char*);
    return (*(Fn*)(*(BYTE**)backend + 0x138))(backend, desc, name);
}

void BackendFlush(void* backend)
{
    using Fn = void(__fastcall*)(void*);
    (*(Fn*)(*(BYTE**)backend + 0x1d0))(backend);
}

void HistCapture(EyeHistory& s, BYTE* r)
{
    BYTE* taa = *(BYTE**)(r + kRendTaa);
    for (int i = 0; i < 10; ++i) s.fbo[i] = *(void**)(r + kHistFbo[i]);
    for (int i = 0; i < 5; ++i) s.tex[i] = *(void**)(taa + kTaaTex[i]);
    memcpy(s.index, r + kRendIndex, sizeof(s.index));
    memcpy(s.stableVp, r + kRendStableVp, sizeof(s.stableVp));
    memcpy(s.view, r + kRendView, sizeof(s.view));
    s.taaCounter = *(int*)taa;
}

void HistApply(const EyeHistory& s, BYTE* r)
{
    BYTE* taa = *(BYTE**)(r + kRendTaa);
    for (int i = 0; i < 10; ++i) *(void**)(r + kHistFbo[i]) = s.fbo[i];
    for (int i = 0; i < 5; ++i) *(void**)(taa + kTaaTex[i]) = s.tex[i];
    memcpy(r + kRendIndex, s.index, sizeof(s.index));
    memcpy(r + kRendStableVp, s.stableVp, sizeof(s.stableVp));
    memcpy(r + kRendView, s.view, sizeof(s.view));
    *(int*)taa = s.taaCounter;
}

// Makes sure `s` holds resources matching the ones the engine currently uses
// (same sizes and formats), creating or re-creating them when needed.
bool HistEnsure(EyeHistory& s, BYTE* r, int eye)
{
    BYTE* taa = *(BYTE**)(r + kRendTaa);
    void* backend = *(void**)(r + kRendBackend);
    if (!taa || !backend) return false;
    bool created = false, flushed = false;
    auto flushOnce = [&] { if (!flushed) { BackendFlush(backend); flushed = true; } };
    for (int i = 0; i < 10; ++i) {
        void* active = *(void**)(r + kHistFbo[i]);
        if (!active) return false;
        if (s.fbo[i] && memcmp(s.fbo[i], active, kFboDesc) == 0) continue;
        if (s.fbo[i] && g_releaseFbo) { flushOnce(); g_releaseFbo(s.fbo[i]); }
        alignas(16) BYTE desc[kFboDesc];
        memcpy(desc, active, kFboDesc);
        s.fbo[i] = BackendCreateFbo(backend, desc, "TeardownVR::EyeHistory");
        if (!s.fbo[i]) { Log("Engine: per-eye history: buffer creation failed"); return false; }
        created = true;
    }
    int w = *(int*)(taa + 0x10), h = *(int*)(taa + 0x14);
    bool texOk = true;
    for (int i = 0; i < 5; ++i) {
        void* active = *(void**)(taa + kTaaTex[i]);
        BYTE* mine = (BYTE*)s.tex[i];
        if ((active != nullptr) != (mine != nullptr) || (mine && (*(int*)(mine + 4) != w || *(int*)(mine + 8) != h))) texOk = false;
    }
    if (!texOk) {
        void* saved[5];
        for (int i = 0; i < 5; ++i) {
            saved[i] = *(void**)(taa + kTaaTex[i]);
            if (s.tex[i] && g_releaseTex) { flushOnce(); g_releaseTex(s.tex[i]); }
            *(void**)(taa + kTaaTex[i]) = nullptr;   // so the engine creates new ones and frees nothing
        }
        g_taaCreate(taa, w, h);
        for (int i = 0; i < 5; ++i) {
            s.tex[i] = *(void**)(taa + kTaaTex[i]);
            *(void**)(taa + kTaaTex[i]) = saved[i];
        }
        created = true;
    }
    if (created) {
        // New buffers: start in phase with the current eye (same jitter / noise
        // sequence and exposure phase) and clear them on first use.
        memcpy(s.index, r + kRendIndex, sizeof(s.index));
        memcpy(s.stableVp, r + kRendStableVp, sizeof(s.stableVp));
        memcpy(s.view, r + kRendView, sizeof(s.view));
        s.taaCounter = *(int*)taa;
        g_histPending[eye] = true;
        Log("Engine: per-eye history buffers ready for eye %d (%dx%d)", eye, w, h);
    }
    s.valid = true;
    return true;
}

// Motion of moving objects in the right eye. Each shape keeps its previous
// render transform (+0x158, next to the current one at +0x13c) for the motion
// vectors; the game rewrites it on every frame (ShapeInterpolationJob), so in the
// zero-time right-eye frame it equals the current one and moving objects get no
// motion in that eye (they smear in its temporal anti-aliasing). The right eye's
// previous frame shows the objects where the left eye's did, so the left eye's
// "previous" transforms are put back for the right eye. Only shapes still in the
// scene are touched (a shape can be deleted between the two frames).
//
// Objects moved by a script during the right-eye frame (syncScriptMotion). That
// frame runs every script's tick again, with no time. A mod that moves an object
// by a fixed step per tick instead of by its time moved it between the two eyes:
// the AC130 plane goes 0.15 m forward per tick (AdvanceGameplayPlane), and was seen
// double, as with a wrong eye separation. The left eye's current render transform
// (+0x13c) is put back for the right eye too (with its previous one), for every
// shape a script moved in between: both eyes see the world in the same state, and
// the object is where the next left-eye frame puts it. Shapes created during the
// right-eye frame are left as they are.
constexpr uint32_t kSceneShapes = 0x238;  // scene -> shape list { int count; ...; Shape** items (+8) }
constexpr uint32_t kShapeFlags = 0xc8;    // shape -> flags (0x20: visible, static shapes too; 0x40: player's tool). ShapeInterpolationJob rewrites +0x13c/+0x158 when & 0x60
constexpr uint32_t kShapeCurXf = 0x13c;   // shape -> current render transform (pos + quat)
constexpr uint32_t kShapeOldXf = 0x158;   // shape -> previous render transform (pos + quat)
constexpr size_t kXfSize = 7 * sizeof(float);
struct EyeAXf { BYTE* shape; float cur[7]; float old[7]; };
std::vector<EyeAXf> g_eyeA;                    // every shape as the left eye drew it, in list order
std::unordered_map<BYTE*, size_t> g_eyeAIndex; // (built only if the list order changed)
// The same index without allocations (fastObjectMotion): open addressing on the shape pointer,
// its table kept from one build to the next (debris: 2.3k-8.8k shapes, rebuilt whenever a
// shape is removed during the pair).
struct ShapeIndex
{
    std::vector<BYTE*> keys;
    std::vector<uint32_t> vals;
    size_t mask = 0;
    bool built = false;

    static size_t Hash(const BYTE* p) { return (size_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 20); }
    void Build(const std::vector<EyeAXf>& a)
    {
        size_t cap = 64;
        while (cap < a.size() * 2) cap <<= 1;
        if (keys.size() != cap) {
            keys.assign(cap, nullptr);
            vals.resize(cap);
        } else {
            std::fill(keys.begin(), keys.end(), nullptr);
        }
        mask = cap - 1;
        for (size_t k = 0; k < a.size(); ++k) {
            BYTE* s = a[k].shape;
            if (!s) continue;
            size_t i = Hash(s) & mask;
            while (keys[i] && keys[i] != s) i = (i + 1) & mask;
            keys[i] = s;
            vals[i] = (uint32_t)k;   // (the last one, as the map did)
        }
        built = true;
    }
    bool Find(const BYTE* s, size_t& k) const
    {
        for (size_t i = Hash(s) & mask;; i = (i + 1) & mask) {
            if (!keys[i]) return false;
            if (keys[i] == s) {
                k = vals[i];
                return true;
            }
        }
    }
};
ShapeIndex g_eyeAFast;
uint64_t g_eyeAFrame = 0;                      // engine frame of g_eyeA (0 = used up)
uint64_t g_scriptMovedTotal = 0;
int g_diagMoved = 0;
uint32_t g_offGameScene = 0x50;           // Game -> scene
// Scene::preRender starts ShapeInterpolationJob on worker threads (they rewrite +0x13c
// and +0x158) and does not wait. Renderer::render waits for them itself
// (Scene::waitShapeInterpolation, render+0x4a7), but only after this hook. The same call
// made earlier lets the left eye record, and the right eye write, finished transforms.
using PFN_WaitShapes = void(__fastcall*)(BYTE* scene);
PFN_WaitShapes g_waitShapes = nullptr;

// ---------------------------------------------------------------------------
// Render-only frame B without shape interpolation jobs ([Performance] skipShapeJobsB)
// ---------------------------------------------------------------------------
// Scene::preRender (0xeb160) builds and starts a ShapeInterpolationJob group in every rendered frame
// (0xd2220: up to 15 jobs, 0xd5a50 each). For each shape with flags & 0x60 a job writes three fields:
// +0x158 = the +0x13c it replaces (the new value instead in screenshot mode, game+0x8a0, or for a
// shape not interpolated yet: +0x174 = 0), +0x13c = the render transform of its body now (0x4de540),
// +0x174 = 1. 0x4de540 also updates the shape's interpolation history (+0x60..+0xa3, 0x322c0: physics
// step, steps since the last one, previous and target transforms, frame stamp) when its body is
// dynamic (+0x138), has a type-0xc parent or the 0x400 flag. With no physics step and no body moved
// since frame A's jobs, that call only writes the new frame number in the stamp (+0xa0), and the
// next frame's call takes the same branch with either stamp (it compares it with its own frame
// number, different from both): skipped too. The step counter is compared below (a frame B whose
// fixed-step loop ran a step after all starts the jobs); skipShapeJobsB=2 checks the rest.
// In a render-only frame B (frameBMode 2: no update and no physics
// step since frame A) FixObjectMotion then gives every shape the left eye drew the left eye's +0x13c
// and previous +0x158 (its +0x13c with a history shared by both eyes) whatever the job wrote, as long
// as the job found +0x13c as the left eye drew it: a shape the job moved is put back, a shape it did
// not move has those values already. So frame B does not start the jobs (their setup on the main
// thread, up to 15 worker wake-ups and a pass over every shape on the workers), and FixObjectMotion
// writes that result itself. The job's own work (the engine's transform function, the same three
// writes) is done on the main thread for the shapes whose result could depend on it: one the left
// eye's record does not hold (created since), one not interpolated yet, one whose +0x13c changed
// after the record (the other arm: OffArmLateFix). If FixObjectMotion stops before its right-eye
// pass, Hook_Render does that work for every shape, as the jobs would have. The waits accept it:
// Scene::waitShapeInterpolation (0xf4150: Renderer::render at 0x883e7, g_waitShapes) finds frame A's
// group, finished; Game::render's wait at 0x71199 (0xf4110) is another group (scene+0xa0,
// PlanningTask) that preRender still starts. skipShapeJobsB=2 checks it: each shape's result is
// compared with what the jobs and FixObjectMotion's rules would have made of the state before (log).
using PFN_ShapeJobs = void(__fastcall*)(BYTE* scene);
using PFN_ShapeXf = float*(__fastcall*)(BYTE* shape, float* out);
PFN_ShapeJobs Real_ShapeJobs = nullptr;  // 0xd2220: build and start the scene's ShapeInterpolationJob group
PFN_ShapeXf g_shapeXf = nullptr;         // 0x4de540: a shape's render transform now (position, quaternion)
BYTE* g_shapeJobsRet = nullptr;          // return address of its call in Scene::preRender (+eb1e1)
BYTE** g_gameGlobal = nullptr;           // the engine's Game pointer, which the job's screenshot test reads
uint32_t g_offScreenshot = 0;            // Game -> screenshot mode ("game.screenshot", set in Game::render)
constexpr uint32_t kShapeInterp = 0x174; // shape -> 1 once the job wrote its transforms (0x4ddc60 clears it)
// shape -> interpolation history of 0x4de540 (0x322c0): step, step count, previous and target transforms
// (0x40 bytes), then the frame stamp (+0xa0, not compared by the check)
constexpr uint32_t kShapeHist = 0x60;
constexpr size_t kShapeHistCmp = 0x40;
// The physics step counter (game+g_gameStep) when preRender last started the jobs, and that frame: frame
// B skips them only right after frame A's, with no physics step in between.
int g_shapeJobsStep = INT_MIN;
uint64_t g_shapeJobsFrame = 0;
struct ShapeJobSkip
{
    uint64_t frame = 0;      // engine frame B whose jobs were not started (0: none)
    BYTE* scene = nullptr;
    bool done = false;       // their work done (by FixObjectMotion, else by Hook_Render)
    uint64_t frames = 0, byHand = 0, fallbacks = 0, checked = 0, diffs = 0, histDiffs = 0;
    uint64_t logFrames = 0, logByHand = 0;   // (since the last summary line)
    bool logged = false;
    // skipShapeJobsB=2: every shape as preRender found it
    struct Pre { BYTE* shape; float cur[7]; float old[7]; BYTE interp; };
    std::vector<Pre> pre;
};
ShapeJobSkip g_shapeSkip;

// 0 off, 1 on, 2 on and checked: the control file TeardownVR_perf.txt ("skipShapeJobsB=N") first.
int ShapeJobsMode()
{
    const int f = FrameB::FileShapeJobsB();
    return std::clamp(f >= 0 ? f : Cfg().skipShapeJobsB, 0, 2);
}

// The job's work for one shape (0xd5a8c-0xd5b2d, matched at start), screenshot mode off (the engine's
// transform function, its interpolation history update included).
int ShapeJobByHand(BYTE* s)
{
    if (!(s[kShapeFlags] & 0x60)) return 0;
    float xf[8];
    g_shapeXf(s, xf);
    memcpy(s + kShapeOldXf, s[kShapeInterp] ? (const float*)(s + kShapeCurXf) : xf, kXfSize);
    memcpy(s + kShapeCurXf, xf, kXfSize);
    s[kShapeInterp] = 1;
    return 1;
}

// Every shape (FixObjectMotion did not reach its right-eye pass): as the jobs would have.
void ShapeJobsByHandAll()
{
    g_shapeSkip.done = true;
    g_shapeSkip.pre.clear();
    ++g_shapeSkip.fallbacks;
    BYTE* list = g_shapeSkip.scene ? *(BYTE**)(g_shapeSkip.scene + kSceneShapes) : nullptr;
    if (!list) return;
    const int n = *(int*)list;
    BYTE** items = *(BYTE***)(list + 8);
    if (n <= 0 || n > 4000000 || !items) return;
    for (int i = 0; i < n; ++i)
        if (items[i]) g_shapeSkip.byHand += ShapeJobByHand(items[i]);
}

void __fastcall Hook_ShapeJobs(BYTE* scene)
{
    // Only Scene::preRender's call (the scene's creation makes one too), in a render-only frame B whose
    // frame A FixObjectMotion recorded just before, with its script-motion rule on (it is what puts
    // back a shape the job would have moved) and screenshot mode off, with the physics step counter
    // still where frame A's jobs found it (the interpolation history then gets nothing but a stamp).
    if (_ReturnAddress() != g_shapeJobsRet) return Real_ShapeJobs(scene);
    const int step = g_gameObj && g_gameStep ? *(const int*)(g_gameObj + g_gameStep) : INT_MIN;
    if (g_rightThisFrame && g_frameBRenderOnly == g_frameCounter && g_vrActive && g_ready && g_historyOk &&
        Cfg().syncScriptMotion && g_eyeAFrame && g_frameCounter == g_eyeAFrame + 1 && step != INT_MIN &&
        step == g_shapeJobsStep && g_frameCounter == g_shapeJobsFrame + 1 && g_gameObj &&
        *g_gameGlobal == g_gameObj && scene && scene == *(BYTE**)(g_gameObj + g_offGameScene) &&
        !g_gameObj[g_offScreenshot]) {
        const int mode = ShapeJobsMode();
        if (mode) {
            g_shapeSkip.frame = g_frameCounter;
            g_shapeSkip.scene = scene;
            g_shapeSkip.done = false;
            g_shapeSkip.pre.clear();
            BYTE* list = *(BYTE**)(scene + kSceneShapes);
            const int n = list ? *(int*)list : 0;
            BYTE** items = list ? *(BYTE***)(list + 8) : nullptr;
            if (mode == 2 && items && n > 0 && n <= 4000000) {
                g_shapeSkip.pre.resize(n);
                for (int i = 0; i < n; ++i) {
                    ShapeJobSkip::Pre& p = g_shapeSkip.pre[i];
                    p.shape = items[i];
                    if (!p.shape) continue;
                    memcpy(p.cur, p.shape + kShapeCurXf, kXfSize);
                    memcpy(p.old, p.shape + kShapeOldXf, kXfSize);
                    p.interp = p.shape[kShapeInterp];
                }
            }
            return;
        }
    }
    Real_ShapeJobs(scene);
    g_shapeJobsStep = step;
    g_shapeJobsFrame = g_frameCounter;
}

// skipShapeJobsB=2: each shape's result against the jobs' work followed by FixObjectMotion's rules,
// applied to the state preRender found (the rules of its right-eye pass, written again here: keep
// them in step). A difference is logged (the first ones) and counted. The transform function is
// called here as the job would have (it updates the shape's interpolation history: in this mode
// the history ends as without the skip); anything in that history but its frame stamp changing by
// that call means skipShapeJobsB=1 would have left a different history for the next frame (a body
// moved since frame A's jobs): counted as a difference too.
void ShapeSkipCheck(bool motion, bool positions)
{
    const std::vector<ShapeJobSkip::Pre>& pre = g_shapeSkip.pre;
    std::unordered_map<BYTE*, size_t> index;
    for (size_t k = 0; k < g_eyeA.size(); ++k)
        if (g_eyeA[k].shape) index[g_eyeA[k].shape] = k;
    int logs = 0;
    for (size_t i = 0; i < pre.size(); ++i) {
        BYTE* s = pre[i].shape;
        if (!s) continue;
        const bool interp = (s[kShapeFlags] & 0x60) != 0;
        float cur[7], old[7];
        memcpy(cur, pre[i].cur, kXfSize);
        memcpy(old, pre[i].old, kXfSize);
        if (interp) {
            BYTE hist[kShapeHistCmp];
            memcpy(hist, s + kShapeHist, sizeof(hist));
            float xf[8];
            g_shapeXf(s, xf);
            memcpy(old, pre[i].interp ? pre[i].cur : xf, kXfSize);
            memcpy(cur, xf, kXfSize);
            if (memcmp(hist, s + kShapeHist, sizeof(hist)) != 0) {
                ++g_shapeSkip.histDiffs;
                ++g_shapeSkip.diffs;
                if (logs++ < 3 && g_shapeSkip.diffs <= 30)
                    Log("Engine: skipShapeJobsB CHECK: shape h%d (flags %02x): the job would have changed its "
                        "interpolation history (step %d -> %d) - a body moved since frame A's jobs",
                        *(int*)(s + 0xc), s[kShapeFlags], *(const int*)hist, *(const int*)(s + kShapeHist));
            }
        }
        const EyeAXf* e = nullptr;
        if (i < g_eyeA.size()) {   // (beyond: appended, FixObjectMotion leaves it)
            if (g_eyeA[i].shape == s) {
                e = &g_eyeA[i];
            } else {
                auto it = index.find(s);
                if (it != index.end()) e = &g_eyeA[it->second];
            }
        }
        if (e) {
            if (positions && memcmp(cur, e->cur, kXfSize) != 0 && memcmp(old, e->cur, kXfSize) == 0) {
                memcpy(cur, e->cur, kXfSize);
                memcpy(old, motion ? e->old : e->cur, kXfSize);
            } else if (motion && interp && memcmp(old, e->old, kXfSize) != 0 && memcmp(old, e->cur, kXfSize) == 0) {
                memcpy(old, e->old, kXfSize);
            }
        }
        ++g_shapeSkip.checked;
        const BYTE wantInterp = interp ? 1 : pre[i].interp;
        if (memcmp(cur, s + kShapeCurXf, kXfSize) == 0 && memcmp(old, s + kShapeOldXf, kXfSize) == 0 &&
            s[kShapeInterp] == wantInterp)
            continue;
        ++g_shapeSkip.diffs;
        if (logs++ < 3 && g_shapeSkip.diffs <= 30) {
            const float* c = (const float*)(s + kShapeCurXf);
            const float* o = (const float*)(s + kShapeOldXf);
            Log("Engine: skipShapeJobsB CHECK: shape h%d (flags %02x, %s) differs from the jobs' result: current "
                "%.4f %.4f %.4f (jobs %.4f %.4f %.4f), previous %.4f %.4f %.4f (jobs %.4f %.4f %.4f)",
                *(int*)(s + 0xc), s[kShapeFlags], e ? "drawn by the left eye" : "new", c[0], c[1], c[2], cur[0], cur[1],
                cur[2], o[0], o[1], o[2], old[0], old[1], old[2]);
        }
    }
}

// End of FixObjectMotion's right-eye pass in place of the jobs: counters, the check, the log.
void ShapeSkipDone(int shapes, int computed, bool motion, bool positions)
{
    ++g_shapeSkip.frames;
    ++g_shapeSkip.logFrames;
    g_shapeSkip.byHand += computed;
    g_shapeSkip.logByHand += computed;
    if (!g_shapeSkip.pre.empty()) {
        ShapeSkipCheck(motion, positions);
        g_shapeSkip.pre.clear();
    }
    if (!g_shapeSkip.logged) {
        g_shapeSkip.logged = true;
        Log("Engine: render-only frame B without shape interpolation jobs (skipShapeJobsB=%d): %d shapes, %d of them "
            "computed here, the others keep the left eye's transforms as FixObjectMotion gives them",
            ShapeJobsMode(), shapes, computed);
    }
    if (g_shapeSkip.logFrames >= 3000) {
        char chk[160] = "";
        if (g_shapeSkip.checked)
            snprintf(chk, sizeof(chk), "; checked %llu shapes: %llu different (%llu of them in the interpolation history)",
                     (unsigned long long)g_shapeSkip.checked, (unsigned long long)g_shapeSkip.diffs,
                     (unsigned long long)g_shapeSkip.histDiffs);
        Log("Engine: skipShapeJobsB: %llu frames B without shape jobs (%llu in all), %.1f shapes per frame computed "
            "here, %llu done by Hook_Render%s", (unsigned long long)g_shapeSkip.logFrames,
            (unsigned long long)g_shapeSkip.frames, (double)g_shapeSkip.logByHand / (double)g_shapeSkip.logFrames,
            (unsigned long long)g_shapeSkip.fallbacks, chk);
        g_shapeSkip.logFrames = g_shapeSkip.logByHand = 0;
    }
}

void FixObjectMotion(BYTE* scene, int eye, bool motion, bool positions)
{
    if (!scene || !g_gameObj) return;
    if (scene != *(BYTE**)(g_gameObj + g_offGameScene)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("Engine: render scene not recognised - right-eye object motion left as is");
        }
        return;
    }
    if (g_waitShapes) {
        const double w0 = g_perfTrace ? ProfNow() : 0;
        g_waitShapes(scene);   // returns at once when the jobs are done
        if (g_perfTrace) g_prof[P_ShapeWait] += ProfNow() - w0;
    }
    BYTE* list = *(BYTE**)(scene + kSceneShapes);
    if (!list) return;
    int n = *(int*)list;
    BYTE** items = *(BYTE***)(list + 8);
    if (n <= 0 || n > 4000000 || !items) return;
    static int logs = 0;
    // fastObjectMotion: the render transforms of the shape 8 places ahead are fetched while this one
    // is compared (+0x13c and +0x158: two cache lines, just written by the interpolation jobs).
    const bool fast = Cfg().fastObjectMotion;
    auto prefetch = [&](int i) {
        if (i < n)
            if (const BYTE* p = items[i]) {
                _mm_prefetch((const char*)p + 0x100, _MM_HINT_T0);
                _mm_prefetch((const char*)p + 0x140, _MM_HINT_T0);
            }
    };
    if (eye == 0) {
        g_eyeA.resize(n);
        for (int i = 0; i < n; ++i) {
            if (fast) prefetch(i + 8);
            BYTE* s = items[i];
            EyeAXf& e = g_eyeA[i];
            e.shape = s;
            if (!s) continue;
            memcpy(e.cur, s + kShapeCurXf, kXfSize);
            memcpy(e.old, s + kShapeOldXf, kXfSize);
        }
        g_eyeAIndex.clear();
        g_eyeAFast.built = false;
        g_eyeAFrame = g_frameCounter;
        return;
    }
    // Only the right-eye frame that directly follows the recorded left-eye frame.
    if (!g_eyeAFrame || g_frameCounter != g_eyeAFrame + 1) return;
    g_eyeAFrame = 0;
    // skipShapeJobsB: this frame B's shape interpolation jobs were not started (Hook_ShapeJobs): their
    // result is written here.
    const bool byHand = g_shapeSkip.frame == g_frameCounter && g_shapeSkip.scene == scene && !g_shapeSkip.done;
    if (byHand) g_shapeSkip.done = true;
    int restored = 0, moved = 0, computed = 0;
    for (int i = 0; i < n; ++i) {
        if (fast) prefetch(i + 8);
        BYTE* s = items[i];
        if (!s) continue;
        const EyeAXf* e = nullptr;
        if (i < (int)g_eyeA.size() && g_eyeA[i].shape == s) {
            e = &g_eyeA[i];
        } else if (i >= (int)g_eyeA.size()) {
            if (byHand) computed += ShapeJobByHand(s);   // (the job's work; nothing more here, as after the job)
            continue;   // appended during the right-eye frame (a removal moves the last shape down, never up)
        } else if (fast) {
            if (!g_eyeAFast.built) g_eyeAFast.Build(g_eyeA);
            size_t k;
            if (!g_eyeAFast.Find(s, k)) {
                if (byHand) computed += ShapeJobByHand(s);
                continue;   // created during the right-eye frame
            }
            e = &g_eyeA[k];
        } else {
            if (g_eyeAIndex.empty())
                for (size_t k = 0; k < g_eyeA.size(); ++k)
                    if (g_eyeA[k].shape) g_eyeAIndex[g_eyeA[k].shape] = k;
            auto it = g_eyeAIndex.find(s);
            if (it == g_eyeAIndex.end()) {
                if (byHand) computed += ShapeJobByHand(s);
                continue;   // created during the right-eye frame
            }
            e = &g_eyeA[it->second];
        }
        if (byHand && (s[kShapeFlags] & 0x60)) {
            if (positions && s[kShapeInterp] && memcmp(s + kShapeCurXf, e->cur, kXfSize) == 0) {
                // The job would have moved this +0x13c (the left eye's) to +0x158 and written its body's
                // transform to +0x13c; the checks below would then leave the left eye's transform and its
                // previous one (the same one with a shared history), whatever the job wrote.
                const float* old = motion ? e->old : e->cur;
                if (memcmp(s + kShapeOldXf, old, kXfSize) != 0) memcpy(s + kShapeOldXf, old, kXfSize);
                continue;
            }
            computed += ShapeJobByHand(s);   // (then the checks below, as after the job)
        }
        // ShapeInterpolationJob (0xd5a50) copies the transform the left eye drew to +0x158
        // before writing the new one: requiring it skips a new shape created at a recycled
        // address (its +0x158 is its own new transform) and frames where the job did not run.
        if (positions && memcmp(s + kShapeCurXf, e->cur, kXfSize) != 0 &&
            memcmp(s + kShapeOldXf, e->cur, kXfSize) == 0) {
            if (Cfg().debugLua && g_diagMoved < 40 && moved < 8) {
                // (diagnostic: which shapes move between the eyes, and by how much)
                ++g_diagMoved;
                const float* c = (const float*)(s + kShapeCurXf);
                const float* o = (const float*)(s + kShapeOldXf);
                BYTE* body = *(BYTE**)(s + 0x10);
                float dB = std::sqrt((c[0] - e->cur[0]) * (c[0] - e->cur[0]) + (c[1] - e->cur[1]) * (c[1] - e->cur[1]) +
                                     (c[2] - e->cur[2]) * (c[2] - e->cur[2]));
                float dA = std::sqrt((e->cur[0] - e->old[0]) * (e->cur[0] - e->old[0]) + (e->cur[1] - e->old[1]) * (e->cur[1] - e->old[1]) +
                                     (e->cur[2] - e->old[2]) * (e->cur[2] - e->old[2]));
                float dOB = std::sqrt((o[0] - e->cur[0]) * (o[0] - e->cur[0]) + (o[1] - e->cur[1]) * (o[1] - e->cur[1]) +
                                      (o[2] - e->cur[2]) * (o[2] - e->cur[2]));
                Log("Engine diag: shape h%d flags %02x body h%d (type %d flags %02x) moved %.4f m between eyes; "
                    "left eye moved %.4f m since its previous frame; right eye's previous vs left current %.4f m; pos %.2f %.2f %.2f",
                    *(int*)(s + 0xc), s[kShapeFlags], body ? *(int*)(body + 0xc) : 0, body ? *(uint16_t*)(body + 8) : 0,
                    body ? body[kShapeFlags] : 0, dB, dA, dOB, c[0], c[1], c[2]);
            }
            memcpy(s + kShapeCurXf, e->cur, kXfSize);
            // With a history per eye, this eye's previous image showed the left eye's previous
            // state; with a shared history, the previous image IS the left eye's (no motion).
            memcpy(s + kShapeOldXf, motion ? e->old : e->cur, kXfSize);
            ++moved;
        } else if (motion && (s[kShapeFlags] & 0x60) && memcmp(s + kShapeOldXf, e->old, kXfSize) != 0 &&
                   memcmp(s + kShapeOldXf, e->cur, kXfSize) == 0) {
            memcpy(s + kShapeOldXf, e->old, kXfSize);
            ++restored;
        }
    }
    if (moved && !g_scriptMovedTotal)
        Log("Engine: %d shapes moved by a script during the right-eye frame - shown where the left eye saw them", moved);
    g_scriptMovedTotal += moved;
    if (byHand) ShapeSkipDone(n, computed, motion, positions);
    if (g_perfTrace) Perf::NoteObjectMotion(n, moved);
    if (Cfg().debugLua && (logs < 3 || ++logs % 2000 == 0)) {
        if (logs < 3) ++logs;
        Log("Engine: right-eye object motion: %d shapes, %d moving restored, %d moved by scripts put back (%llu in all)",
            n, restored, moved, (unsigned long long)g_scriptMovedTotal);
    }
}

// ---------------------------------------------------------------------------
// The hand that holds no tool
// ---------------------------------------------------------------------------
// It is drawn too (main.lua, tdvrOffArmTick): the hand and forearm of the player's
// character, two static bodies the Lua mod moves in each left-eye frame to where the
// controller is, for the eye camera it asks the game for. The camera actually drawn is
// that one moved by what the player moved after the scripts' tick (physics steps, the
// camera's own smoothing): the arm stayed behind while walking (up to one physics step,
// ~7 cm at 4.5 m/s), while the tool, placed by the engine relative to the player, does
// not. Static bodies are drawn exactly at their body transform (the shape interpolation
// job, 0xd5a50, rewrites +0x13c of every visible shape in every frame with 0x4de540,
// which interpolates only dynamic bodies: body+0x138 = 0 here), so once the job is done
// the arm's render transforms (+0x13c) are moved by the difference between the two
// cameras: the arm is drawn where the controller is relative to the eyes. The right eye
// gets the left eye's transforms (and, for its motion vectors, the previous pair's).
// The Lua mod sends the body handles (TDVRW) and, in each frame it places the arm, its
// eye camera minus the hand body's position (TDVRE). Bodies are found from their script
// handles with the engine's own lookup (SetShapeLocalTransform's binding, 0x496410):
// positive handles index scene+0xa20 (count +0xa18), negative ones scene+0x210 (count
// +0x208); an entity keeps its handle at +0x0c and its type at +0x08 (1 body, 2 shape).
static uint32_t g_entPosCount = 0, g_entPosArray = 0, g_entNegCount = 0, g_entNegArray = 0;

static bool EntityLookupReady() { return g_entPosArray != 0; }

static BYTE* EntityOfHandle(BYTE* scene, int h)
{
    if (!scene || !g_entPosArray || h == 0) return nullptr;
    const unsigned i = h > 0 ? (unsigned)h : 0u - (unsigned)h;
    const int count = *(const int*)(scene + (h > 0 ? g_entPosCount : g_entNegCount));
    if (count <= 0 || i >= (unsigned)count) return nullptr;
    BYTE** list = *(BYTE***)(scene + (h > 0 ? g_entPosArray : g_entNegArray));
    BYTE* e = list ? list[i] : nullptr;
    return e && *(const int*)(e + kEntityHandle) == h ? e : nullptr;
}

static void InitOffArm()
{
    // In the binding, after ArgInt: the script handle's lookup in the scene (the game's +50h): a positive handle
    // indexes the list at +A20h (count +A18h), a negative one the list at +210h (count +208h) (pattern below).
    BYTE* fn = FindLuaBinding("SetShapeLocalTransform");
    BYTE* m = fn ? ScanIn(fn, 0x100, "4D 8B 50 ?? 85 C0 7E ?? 45 3B 8A ?? ?? 00 00 7D ?? 49 8B 82 ?? ?? 00 00 4A 8B 1C C8 "
                                     "EB ?? 79 ?? 41 8B C1 F7 D8 41 3B 82 ?? ?? 00 00 7D ?? 49 8B C9 48 C1 E1 03 "
                                     "49 8B 82 ?? ?? 00 00")
                 : nullptr;
    if (!m || m[3] != g_offGameScene) {
        Log("Engine: script handle lookup not recognised (binding %p) - the other arm is placed by the Lua mod alone", fn);
        return;
    }
    g_entPosCount = U32(m + 11);
    g_entPosArray = U32(m + 20);
    g_entNegCount = U32(m + 40);
    g_entNegArray = U32(m + 56);
    Log("Engine: script handles -> entities (+%x/+%x, +%x/+%x): the other arm is drawn on the camera drawn",
        g_entPosCount, g_entPosArray, g_entNegCount, g_entNegArray);
}

// Late fix of a group of static bodies the Lua mod places itself in the left-eye frames: the arm
// (hand and forearm: TDVRW / TDVRE), and the off hand's gun copy of a dual-wield tool (main.lua,
// tdvrDualTick: TDVRV / TDVRX; it follows the drawn hand the same way). Each group has its own
// reference (its first body) and camera message.
constexpr int kOffArmShapes = 8;
struct OffArmFix
{
    const char* what = "";                // (log)
    int bodies[2] = {};                   // hand and forearm (TDVRW); the gun copy: bodies[0] only (TDVRV)
    Vec3 camRel;                          // eye camera the Lua mod placed them for - first body (TDVRE / TDVRX)
    uint64_t camFrame = 0;                // engine frame of camRel
    uint64_t pairFrame = 0;               // left-eye frame of the pair being drawn (0 = not moved)
    int n = 0;
    BYTE* shape[kOffArmShapes] = {};
    float cur[kOffArmShapes][7] = {};     // their render transforms in this pair
    int prevN = 0;
    BYTE* prevShape[kOffArmShapes] = {};
    float prev[kOffArmShapes][7] = {};    // in the previous pair (the right eye's previous image)
    bool logged = false;
    float maxMove = 0, sumMove = 0;       // diagnostics (debugLua)
    int moves = 0;
};
static OffArmFix g_offArm, g_dualCopyFix;

// The group's shapes (its bodies'), at most kOffArmShapes.
static int OffArmShapes(BYTE* scene, const OffArmFix& a, BYTE** out)
{
    int n = 0;
    for (int b = 0; b < 2; ++b) {
        BYTE* body = EntityOfHandle(scene, a.bodies[b]);
        if (!body || *(const uint16_t*)(body + kEntityType) != 1) continue;
        ForToolShapes(body, [&](BYTE* sh) {
            if (n < kOffArmShapes) out[n++] = sh;
        });
    }
    return n;
}

static void OffArmLateFix(BYTE* scene, int eye, OffArmFix& a)
{
    if (!g_entPosArray || !g_waitShapes || !g_gameObj || !scene || scene != *(BYTE**)(g_gameObj + g_offGameScene)) return;
    BYTE* sh[kOffArmShapes] = {};
    if (eye == 0) {
        a.pairFrame = 0;
        // Only when the Lua mod placed the group in this frame, for the head view drawn now
        // (synchronised stereo; not a mod's camera, not the map).
        if (!g_leftThisFrame || a.camFrame != g_frameCounter || g_anchorFrame != g_frameCounter || !g_leftCamValid ||
            g_script.lastFrame == g_frameCounter || g_map.on) {
            a.prevN = 0;
            return;
        }
        BYTE* ref = EntityOfHandle(scene, a.bodies[0]);
        if (!ref || *(const uint16_t*)(ref + kEntityType) != 1) return;
        const float* hb = (const float*)(ref + 0x40);   // body: world position, then rotation
        const Vec3 d = g_leftCam.p - (Vec3{ hb[0], hb[1], hb[2] } + a.camRel);
        if (!(Length(d) < 0.5f)) return;   // (a teleport, a message from another frame; NaN)
        const int n = OffArmShapes(scene, a, sh);
        if (!n) return;
        g_waitShapes(scene);   // (the shape interpolation job has written +0x13c)
        // A +0x13c still as this fix left it: the job did not rewrite it in this frame (a
        // shape it skipped, a second render): moving it again would add up.
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < a.n; ++k)
                if (a.shape[k] == sh[i] && memcmp(sh[i] + kShapeCurXf, a.cur[k], kXfSize) == 0) return;
        for (int i = 0; i < n; ++i) {
            float* c = (float*)(sh[i] + kShapeCurXf);
            c[0] += d.x;
            c[1] += d.y;
            c[2] += d.z;
            a.shape[i] = sh[i];
            memcpy(a.cur[i], c, kXfSize);
        }
        a.n = n;
        a.pairFrame = g_frameCounter;
        if (!a.logged) {
            a.logged = true;
            Log("Engine: %s is drawn on the camera drawn (%d shapes, moved %.1f mm)", a.what, n, Length(d) * 1000.0f);
        }
        if (Cfg().debugLua) {
            a.maxMove = std::max(a.maxMove, Length(d));
            a.sumMove += Length(d);
            if (++a.moves == 600) {
                Log("Engine: %s moved onto the camera drawn over 600 pairs: max %.1f mm, mean %.1f mm (%d shapes)", a.what,
                    a.maxMove * 1000.0f, a.sumMove / 600 * 1000.0f, n);
                a.maxMove = a.sumMove = 0;
                a.moves = 0;
            }
        }
        return;
    }
    if (!a.pairFrame || g_frameCounter != a.pairFrame + 1) return;
    a.pairFrame = 0;
    const int n = OffArmShapes(scene, a, sh);
    if (n != a.n) return;
    for (int i = 0; i < n; ++i)
        if (sh[i] != a.shape[i]) return;
    g_waitShapes(scene);
    // With a history per eye, this eye's previous image showed the previous pair's arm;
    // with a shared one, the previous image is the left eye's (no motion).
    const bool motion = Cfg().perEyeHistory && g_histActive == 1;
    for (int i = 0; i < n; ++i) {
        const float* old = a.cur[i];
        if (motion)
            for (int k = 0; k < a.prevN; ++k)
                if (a.prevShape[k] == sh[i]) old = a.prev[k];
        memcpy(sh[i] + kShapeCurXf, a.cur[i], kXfSize);
        memcpy(sh[i] + kShapeOldXf, old, kXfSize);
    }
    memcpy(a.prevShape, a.shape, sizeof(a.shape));
    memcpy(a.prev, a.cur, sizeof(a.cur));
    a.prevN = n;
}

static void OffArmLateFix(BYTE* scene, int eye)
{
    g_offArm.what = "the other arm";
    g_dualCopyFix.what = "the off hand's gun (dual wield)";
    OffArmLateFix(scene, eye, g_offArm);
    OffArmLateFix(scene, eye, g_dualCopyFix);
}

// ---------------------------------------------------------------------------
// Dead post-processing passes ([Performance] skipDeadPasses)
// ---------------------------------------------------------------------------
// With the mod's removeBloom (SetPostProcessingProperty("bloom", 0): scene+0xa4c = 0) and removeDof
// (SetCameraDof(0, 0) at every tick: Dof+8 = 0, and the one-shot flag renderer+0x23f0 that makes Renderer::render
// run the depth of field whatever options.gfx.dof says) two passes compute what nothing visible uses.
//  - Bloom::render (+7ea80, reached through +7f050, which only counts the calls at bloom+0xb0, read by nothing):
//    20 fullscreen draws into Bloom::mMipFbo[7][2] and Bloom::mResultFbo (+0x38, cleared, then its 7 levels
//    added). The result's only readers are the post pass (+87070: colour = composite + bloom * mubPostParams.z,
//    z = scene+0xa4c, times game.screenshot.bloom in screenshot mode) and the debug view "bloom" (renderer+0x978).
//    With z == 0 and a finite bloom texel, bloom * 0 = +/-0 and composite + (+/-0) = composite (D3D float rules;
//    a -0 / +0 difference ends in exp2(+/-0) = 1 anyway). Skipped: the result target is cleared to 0 instead
//    (what the chain itself does first; finite whatever the target held before).
//  - Dof::render (+80ea0): circle of confusion = (...) * Dof+8 = +/-0 or NaN, so no blur loop (its radius goes
//    through max / min, which drop a NaN), each blur output = its centre sample / 1 with alpha 0, and the last pass
//    writes composite = t1 + (t2 - t1) * 0 = t1, the composite's own texel copied through RGBA16F targets of its own
//    size (exact for R11G11B10 values, sampled at texel centres; only an R11G11B10 composite: the pass writes alpha
//    0, which an RGBA16F composite would keep). Skipped: nothing drawn; its one side
//    effect, the noise-angle global (+1c6ecb0, read only by Dof::render), is written as it would have been. Its
//    focus pass (+80da0, its own history, called separately) still runs.
// Both need a frame without Inf / NaN in its HDR image (D3D float conversions saturate to the largest finite value,
// so only a shader's division by 0 could make one, already black or white in the original image).
// skipDeadPasses=2 checks it on the GPU every kDpCheckEvery eligible calls, alternately: the composite copied before
// and after the real Dof::render; the post pass drawn with the bloom result cleared, then with the real one put back
// (the image shown is the original). The copies are the engine's own (TRendererD3D12 +140h, its barrier tracker kept
// right) into targets of ours, read back once the frame's fence passed, compared on the CPU over a few frames. A
// difference is logged and turns the skips off until the game restarts.
using PFN_BloomRender = void(__fastcall*)(BYTE* bloom, void* input, void* exposure, float threshold, float intensity);
using PFN_DofRender = void(__fastcall*)(BYTE* dof, BYTE* fbo, void* depth, void* noise, float depthScale);
using PFN_PostPass = void(__fastcall*)(BYTE* r, void* input);
using PFN_ClearTexture = void(__fastcall*)(void* backend, void* tex, const float* rgba);
using PFN_CopyFbo = void(__fastcall*)(void* backend, void* dstFbo, void* srcFbo, uint32_t attachment, uint64_t range);
PFN_BloomRender Real_BloomRender = nullptr;
PFN_DofRender Real_DofRender = nullptr;
PFN_PostPass Real_PostPass = nullptr;
PFN_ClearTexture g_dpClear = nullptr;    // TRendererD3D12 +60h: clear a texture (backend, texture, colour)
PFN_CopyFbo g_dpCopy = nullptr;          // TRendererD3D12 +140h: copy an FBO's colour texture (backend, dst, src, index, 0)
BYTE** g_dpGame = nullptr;               // the engine's Game pointer (the one the post pass and Dof::render read)
float* g_dpDofAngle = nullptr;           // Dof::render's noise angle
bool g_dpOn = false;                     // the hooks are in place and every pattern matched
bool g_dpOff = false;                    // a check found a difference: no skip until the game restarts
BYTE* g_dpR = nullptr;                   // the renderer whose Renderer::render runs now (Hook_Render), else nullptr
int g_dpLoggedMode = -1;
uint64_t g_dpLogAt = 0;
constexpr uint32_t kDpBloom = 0xa38, kDpDof = 0xa28, kDpScene = 0x2200, kDpSceneBloom = 0xa4c, kDpDebugView = 0x978,
                   kDpComposite = 0xa78, kDpPostFbo = 0xa88;                        // renderer / scene
constexpr uint32_t kDpGameShot = 0x8a0, kDpGamePaused = 0x1e8, kDpGameFrame = 0x1f0;  // game
constexpr uint32_t kDpBloomResult = 0x38, kDpDofAmount = 8, kDpDofBackend = 0x18, kDpDofPrep0 = 0x68,
                   kDpDofBlur0 = 0x78, kDpDofPrep1 = 0x70, kDpDofBlur1 = 0x80;      // Bloom / Dof
constexpr uint32_t kDpFboFormat = 0xc, kDpFboTex = 0x38, kDpTexRes = 0x38;        // FBO -> texture -> ID3D12Resource
constexpr uint32_t kDpBkInFrame = 0xcc, kDpBkList = 0xe40, kDpBkFence = 0xe58, kDpBkNext = 0xe68;  // TRendererD3D12
constexpr uint32_t kDpFmtR11G11B10 = 0x12, kDpFmtRGBA16F = 7;                      // the engine's format numbers
constexpr uint64_t kDpCheckEvery = 900;  // eligible calls of a pass between two checks (2 per frame pair: ~5 s at 90 Hz)
const float kDpClearColour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // Bloom::render's own (+c49070)

enum { kDpWhyMode, kDpWhyAmount, kDpWhyShot, kDpWhyDebug, kDpWhyFrame, kDpWhyTarget, kDpWhyCheck, kDpWhyN };
const char* const kDpWhyName[kDpWhyN] = { "off", "amount not 0", "photo/screenshot mode", "debug view",
                                          "outside a frame", "targets not as expected", "checked" };
struct DpPass
{
    uint64_t skip = 0, run = 0, why[kDpWhyN] = {};   // since the last summary line
    uint64_t skipAll = 0, eligible = 0;
    bool logged = false;
};
DpPass g_dpBloom, g_dpDof;

// The check: our targets (made by the engine from the copied FBO's descriptor) and their readback copies.
struct DpBuf
{
    void* fbo = nullptr;
    BYTE desc[kFboDesc] = {};
    ID3D12Resource* rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 rowBytes = 0, size = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
DpBuf g_dpBuf[5];   // 0 / 1: the composite before / after Dof::render; 2 / 3: post with the bloom cleared / real; 4: the bloom
struct DpCheck
{
    bool ready = false;              // the copy, the fence and the FBO release were found
    int kind = 0;                    // in flight: 0 none, 1 bloom, 2 depth of field
    uint64_t frame = 0;
    ID3D12Fence* fence = nullptr;    // the renderer's frame fence (one reference held while in flight)
    UINT64 value = 0;                // the value endRender signals after this frame's list
    DpBuf *a = nullptr, *b = nullptr, *s = nullptr;   // compared pair; s scanned for Inf / NaN
    const BYTE *pa = nullptr, *pb = nullptr, *ps = nullptr;
    UINT row = 0, srow = 0;
    uint64_t diffs = 0, nonFinite = 0;
    int fx = -1, fy = -1;
    uint32_t fa = 0, fb = 0;
    BYTE* postArmed = nullptr;       // Hook_BloomRender ran a checked chain: Hook_PostPass compares
    uint64_t postFrame = 0;
    uint64_t done[3] = {}, different[3] = {}, nonFiniteFrames = 0;
    int logs[3] = {};
};
DpCheck g_dpChk;

// 0 off, 1 on, 2 on and checked: the control file TeardownVR_perf.txt ("skipDeadPasses=N") first.
int DeadPassMode()
{
    const int f = FrameB::FileDeadPasses();
    return g_dpOff ? 0 : std::clamp(f >= 0 ? f : Cfg().skipDeadPasses, 0, 2);
}

// Common conditions: not in photo / screenshot mode, the renderer's debug view the normal one (an engine string,
// 32 bytes, inline unless its byte +1f is set: "final" - set by the renderer's constructor at 0x82d90 - or empty;
// "bloom", "fog", "rawlight"... show an intermediate buffer, 0x900b0), the backend inside a frame (TRendererD3D12
// +cc, beginRender .. endRender).
int DpCommon(const BYTE* r, const void* backend)
{
    const BYTE* game = *g_dpGame;
    if (!game || game[kDpGameShot]) return kDpWhyShot;
    const BYTE* s = r + kDpDebugView;
    const char* d = s[0x1f] ? *(const char* const*)s : (const char*)s;
    if (!d || (d[0] && strcmp(d, "final") != 0)) return kDpWhyDebug;
    if (!backend || !((const BYTE*)backend)[kDpBkInFrame]) return kDpWhyFrame;
    return -1;
}

int BloomWhy(const BYTE* r, BYTE* bloom)
{
    if (DeadPassMode() == 0) return kDpWhyMode;
    if (!r || *(BYTE* const*)(r + kDpBloom) != bloom) return kDpWhyTarget;
    const BYTE* scene = *(const BYTE* const*)(r + kDpScene);
    if (!scene || !(*(const float*)(scene + kDpSceneBloom) == 0.0f)) return kDpWhyAmount;
    const int c = DpCommon(r, *(void**)bloom);
    if (c >= 0) return c;
    const BYTE* res = *(const BYTE* const*)(bloom + kDpBloomResult);
    if (!res || !*(void* const*)(res + kDpFboTex)) return kDpWhyTarget;
    return -1;
}

int DofWhy(const BYTE* r, BYTE* dof, BYTE* fbo)
{
    if (DeadPassMode() == 0) return kDpWhyMode;
    if (!r || !fbo || *(BYTE* const*)(r + kDpDof) != dof || *(BYTE* const*)(r + kDpComposite) != fbo) return kDpWhyTarget;
    if (!(*(const float*)(dof + kDpDofAmount) == 0.0f)) return kDpWhyAmount;
    const int c = DpCommon(r, *(void**)(dof + kDpDofBackend));
    if (c >= 0) return c;
    // (the proof: the composite, R11G11B10 only - the last pass writes alpha 0 - goes through RGBA16F targets of its
    // own size; the half-resolution pair must hold an alpha channel, so that t2.a is the blur's 0 and not a format's 1)
    const BYTE* prep = *(const BYTE* const*)(dof + kDpDofPrep0);
    const BYTE* blur = *(const BYTE* const*)(dof + kDpDofBlur0);
    const BYTE* prep1 = *(const BYTE* const*)(dof + kDpDofPrep1);
    const BYTE* blur1 = *(const BYTE* const*)(dof + kDpDofBlur1);
    if (!prep || !blur || !prep1 || !blur1 || *(const uint32_t*)(fbo + kDpFboFormat) != kDpFmtR11G11B10 ||
        *(const uint32_t*)(prep + kDpFboFormat) != kDpFmtRGBA16F || *(const uint32_t*)(blur + kDpFboFormat) != kDpFmtRGBA16F ||
        *(const uint32_t*)(prep1 + kDpFboFormat) != kDpFmtRGBA16F || *(const uint32_t*)(blur1 + kDpFboFormat) != kDpFmtRGBA16F ||
        memcmp(prep, fbo, 8) != 0 || memcmp(blur, fbo, 8) != 0)
        return kDpWhyTarget;
    return -1;
}

// A check now? (counts the pass's eligible calls; one check in flight at a time, the two kinds half a period apart)
bool DpCheckDue(DpPass& p, int kind)
{
    ++p.eligible;
    if (!g_dpChk.ready || g_dpChk.kind || g_dpChk.postArmed) return false;
    return p.eligible % kDpCheckEvery == (kind == 1 ? kDpCheckEvery / 2 : 0);
}

void DpCheckFailed(const char* why)
{
    g_dpChk.ready = false;
    Log("Engine: skipDeadPasses=2: check NOT available (%s) - skips as with 1, unchecked", why);
}

// Our target `c` made like `src` (an engine FBO: its first bytes are its creation descriptor), and its readback buffer.
bool DpEnsure(DpBuf& c, void* backend, const BYTE* src, const char* name)
{
    if (!c.fbo || memcmp(c.desc, src, kFboDesc) != 0) {
        if (c.fbo) {   // (the size or format changed: no check is in flight, nothing of ours is queued)
            BackendFlush(backend);
            g_releaseFbo(c.fbo);
            c.fbo = nullptr;
        }
        if (c.rb) {
            c.rb->Release();
            c.rb = nullptr;
        }
        alignas(16) BYTE desc[kFboDesc];
        memcpy(desc, src, kFboDesc);
        c.fbo = BackendCreateFbo(backend, desc, name);
        if (!c.fbo) return false;
        memcpy(c.desc, src, kFboDesc);
    }
    if (c.rb) return true;
    const BYTE* tex = *(const BYTE* const*)((const BYTE*)c.fbo + kDpFboTex);
    ID3D12Resource* res = tex ? *(ID3D12Resource* const*)(tex + kDpTexRes) : nullptr;
    if (!res) return false;
    ID3D12Device* dev = nullptr;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
    const D3D12_RESOURCE_DESC td = res->GetDesc();
    dev->GetCopyableFootprints(&td, 0, 1, 0, &c.fp, &c.rows, &c.rowBytes, &c.size);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = c.size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&c.rb));
    dev->Release();
    if (FAILED(hr)) {
        c.rb = nullptr;
        return false;
    }
    c.fmt = td.Format;
    return true;
}

// Our target's texture copied into its readback buffer, in the game's command list. `fromDest`: the engine's copy
// left it in COPY_DEST (what its barrier tracker records), put back after ours; else it is in COPY_SOURCE already.
void DpReadback(void* backend, DpBuf& c, bool fromDest)
{
    auto* cl = *(ID3D12GraphicsCommandList* const*)((const BYTE*)backend + kDpBkList);
    const BYTE* tex = *(const BYTE* const*)((const BYTE*)c.fbo + kDpFboTex);
    ID3D12Resource* res = *(ID3D12Resource* const*)(tex + kDpTexRes);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (fromDest) cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = c.rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = c.fp;
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (fromDest) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
    }
}

// The copies are in this frame's list: compared once the renderer's fence passes the value endRender signals after it.
void DpArm(int kind, void* backend, DpBuf* a, DpBuf* b, DpBuf* s)
{
    auto& c = g_dpChk;
    c.fence = *(ID3D12Fence* const*)((const BYTE*)backend + kDpBkFence);
    c.value = *(const UINT64*)((const BYTE*)backend + kDpBkNext);
    if (!c.fence) return;
    c.fence->AddRef();
    c.kind = kind;
    c.frame = g_frameCounter;
    c.a = a;
    c.b = b;
    c.s = s;
    c.pa = c.pb = c.ps = nullptr;
    c.row = c.srow = 0;
    c.diffs = c.nonFinite = 0;
    c.fx = c.fy = -1;
    c.fa = c.fb = 0;
}

void DpDisarm()
{
    auto& c = g_dpChk;
    const D3D12_RANGE none = { 0, 0 };
    if (c.pa) c.a->rb->Unmap(0, &none);
    if (c.pb) c.b->rb->Unmap(0, &none);
    if (c.ps) c.s->rb->Unmap(0, &none);
    c.pa = c.pb = c.ps = nullptr;
    if (c.fence) c.fence->Release();
    c.fence = nullptr;
    c.kind = 0;
}

// An R11G11B10 texel holding an Inf or a NaN (a 5-bit exponent at 31: bits 6-10, 17-21, 27-31).
inline bool DpNonFinite11(uint32_t w)
{
    return ((w >> 6) & 31) == 31 || ((w >> 17) & 31) == 31 || ((w >> 27) & 31) == 31;
}

// One step of the check in flight (main thread, every frame): the comparison spread over frames (~0.25 ms each).
void DpCheckStep()
{
    auto& c = g_dpChk;
    if (c.postArmed && c.postFrame != g_frameCounter) c.postArmed = nullptr;   // (its post pass did not come)
    if (!c.kind) return;
    if (!c.pa) {
        if (c.fence->GetCompletedValue() < c.value) {
            if (g_frameCounter - c.frame > 2000) {
                Log("Engine: skipDeadPasses CHECK abandoned: the frame's GPU work never finished (fence %llu < %llu)",
                    (unsigned long long)c.fence->GetCompletedValue(), (unsigned long long)c.value);
                DpDisarm();
            }
            return;
        }
        D3D12_RANGE ra = { 0, (SIZE_T)c.a->size }, rb = { 0, (SIZE_T)c.b->size }, rs = { 0, (SIZE_T)c.s->size };
        void *pa = nullptr, *pb = nullptr, *ps = nullptr;
        const bool ok = SUCCEEDED(c.a->rb->Map(0, &ra, &pa)) && (c.pa = (const BYTE*)pa) != nullptr &&
                        SUCCEEDED(c.b->rb->Map(0, &rb, &pb)) && (c.pb = (const BYTE*)pb) != nullptr &&
                        SUCCEEDED(c.s->rb->Map(0, &rs, &ps)) && (c.ps = (const BYTE*)ps) != nullptr;
        if (!ok) {
            DpDisarm();
            DpCheckFailed("a readback buffer could not be mapped");
            return;
        }
    }
    const double t0 = ProfNow();
    const UINT w = c.a->fp.Footprint.Width;
    const size_t bpp = w ? (size_t)(c.a->rowBytes / w) : 0;
    while (c.row < c.a->rows) {
        const BYTE* x = c.pa + c.a->fp.Offset + (size_t)c.row * c.a->fp.Footprint.RowPitch;
        const BYTE* y = c.pb + c.b->fp.Offset + (size_t)c.row * c.b->fp.Footprint.RowPitch;
        if (memcmp(x, y, (size_t)c.a->rowBytes) != 0 && bpp)
            for (UINT i = 0; i < w; ++i)
                if (memcmp(x + i * bpp, y + i * bpp, bpp) != 0) {
                    if (!c.diffs++) {
                        c.fx = (int)i;
                        c.fy = (int)c.row;
                        memcpy(&c.fa, x + i * bpp, std::min<size_t>(bpp, 4));
                        memcpy(&c.fb, y + i * bpp, std::min<size_t>(bpp, 4));
                    }
                }
        ++c.row;
        if ((c.row & 15) == 0 && ProfNow() - t0 > 0.25) return;
    }
    while (c.s->fmt == DXGI_FORMAT_R11G11B10_FLOAT && c.srow < c.s->rows) {
        const uint32_t* p = (const uint32_t*)(c.ps + c.s->fp.Offset + (size_t)c.srow * c.s->fp.Footprint.RowPitch);
        for (UINT i = 0; i < c.s->fp.Footprint.Width; ++i) c.nonFinite += DpNonFinite11(p[i]);
        ++c.srow;
        if ((c.srow & 15) == 0 && ProfNow() - t0 > 0.25) return;
    }
    const int k = c.kind;
    const char* what = k == 1 ? "bloom: the post pass with the bloom result cleared vs drawn"
                              : "depth of field: the composite before vs after Dof::render";
    ++c.done[k];
    if (c.nonFinite) ++c.nonFiniteFrames;
    if (c.diffs) {
        ++c.different[k];
        Log("Engine: skipDeadPasses CHECK %s: %llu of %ux%u pixels DIFFERENT (first at %d,%d: %08x vs %08x; Inf/NaN "
            "texels in the %s: %llu) - the skips are turned off until the game restarts", what,
            (unsigned long long)c.diffs, w, c.a->rows, c.fx, c.fy, c.fa, c.fb, k == 1 ? "bloom result" : "composite",
            (unsigned long long)c.nonFinite);
        g_dpOff = true;
    } else if (c.nonFinite) {
        // (identical this time, but the proof needs a finite HDR image: a frame without it is one where a skip
        // could differ from the original, so the skips stop as for a difference)
        Log("Engine: skipDeadPasses CHECK %s: identical, %ux%u pixels, but %llu Inf/NaN texels in the %s (frame %llu) "
            "- the proof's precondition failed: the skips are turned off until the game restarts", what, w, c.a->rows,
            (unsigned long long)c.nonFinite, k == 1 ? "bloom result" : "composite", (unsigned long long)c.frame);
        g_dpOff = true;
    } else if (c.logs[k] < 3) {
        ++c.logs[k];
        Log("Engine: skipDeadPasses CHECK %s: identical, %ux%u pixels (frame %llu; Inf/NaN texels in the %s: 0)",
            what, w, c.a->rows, (unsigned long long)c.frame, k == 1 ? "bloom result" : "composite");
    }
    DpDisarm();
}

void DpWhyText(std::string& s, const DpPass& p)
{
    char b[96];
    for (int i = 0; i < kDpWhyN; ++i)
        if (p.why[i]) {
            snprintf(b, sizeof(b), "%s%s %llu", s.empty() ? "" : ", ", kDpWhyName[i], (unsigned long long)p.why[i]);
            s += b;
        }
}

// Main thread, every Renderer::render (Hook_Render): the mode's log, the summary, the check in flight.
void DeadPassPoll()
{
    if (!g_dpOn) return;
    const int m = DeadPassMode();
    if (m != g_dpLoggedMode) {
        Log("Engine: skipDeadPasses=%d%s: %s", m, FrameB::FileDeadPasses() >= 0 ? " (control file)" : "",
            g_dpOff ? "OFF after a check found a difference (until the game restarts)"
            : m == 0 ? "the bloom chain and the depth of field are drawn as before"
            : m == 1 ? "the bloom chain / depth of field are not drawn while their amount is exactly 0"
                     : "the same, checked on the GPU every ~5 s (exact comparison)");
        g_dpLoggedMode = m;
    }
    DpCheckStep();
    const uint64_t n = g_dpBloom.skip + g_dpBloom.run + g_dpDof.skip + g_dpDof.run;
    if (g_frameCounter - g_dpLogAt < 3000 || n == 0) return;
    g_dpLogAt = g_frameCounter;
    std::string wb, wd;
    DpWhyText(wb, g_dpBloom);
    DpWhyText(wd, g_dpDof);
    char chk[200] = "";
    if (g_dpChk.done[1] + g_dpChk.done[2])
        snprintf(chk, sizeof(chk), "; checks: bloom %llu (%llu different), depth of field %llu (%llu different), with "
                 "Inf/NaN texels %llu", (unsigned long long)g_dpChk.done[1], (unsigned long long)g_dpChk.different[1],
                 (unsigned long long)g_dpChk.done[2], (unsigned long long)g_dpChk.different[2],
                 (unsigned long long)g_dpChk.nonFiniteFrames);
    Log("Engine: skipDeadPasses=%d: bloom chain not drawn %llu, drawn %llu%s%s%s; depth of field not drawn %llu, drawn "
        "%llu%s%s%s (since the last line; not drawn in all: %llu / %llu)%s", m,
        (unsigned long long)g_dpBloom.skip, (unsigned long long)g_dpBloom.run, wb.empty() ? "" : " (", wb.c_str(),
        wb.empty() ? "" : ")", (unsigned long long)g_dpDof.skip, (unsigned long long)g_dpDof.run, wd.empty() ? "" : " (",
        wd.c_str(), wd.empty() ? "" : ")", (unsigned long long)g_dpBloom.skipAll, (unsigned long long)g_dpDof.skipAll, chk);
    for (DpPass* p : { &g_dpBloom, &g_dpDof }) {
        p->skip = p->run = 0;
        for (auto& w : p->why) w = 0;
    }
}

// Bloom::render(bloom, input = the anti-aliased image, exposure, threshold, intensity). Through +7f050 (its call
// count already taken).
void __fastcall Hook_BloomRender(BYTE* bloom, void* input, void* exposure, float threshold, float intensity)
{
    if (!g_dpOn) return Real_BloomRender(bloom, input, exposure, threshold, intensity);
    BYTE* r = g_dpR;
    int why = BloomWhy(r, bloom);
    if (why < 0 && DeadPassMode() == 2 && DpCheckDue(g_dpBloom, 1)) {
        g_dpChk.postArmed = r;   // drawn; Hook_PostPass compares the post pass with and without it
        g_dpChk.postFrame = g_frameCounter;
        why = kDpWhyCheck;
    }
    if (why >= 0) {
        ++g_dpBloom.run;
        ++g_dpBloom.why[why];
        return Real_BloomRender(bloom, input, exposure, threshold, intensity);
    }
    // Dead: its result target holds zeros instead (the post pass adds it times exactly 0).
    const BYTE* res = *(const BYTE* const*)(bloom + kDpBloomResult);
    g_dpClear(*(void**)bloom, *(void* const*)(res + kDpFboTex), kDpClearColour);
    ++g_dpBloom.skip;
    ++g_dpBloom.skipAll;
    if (!g_dpBloom.logged) {
        g_dpBloom.logged = true;
        Log("Engine: skipDeadPasses: bloom chain not drawn (bloom factor exactly 0; its result target cleared instead)");
    }
}

// Dof::render(dof, the composite FBO, depth, noise, depth scale).
void __fastcall Hook_DofRender(BYTE* dof, BYTE* fbo, void* depth, void* noise, float depthScale)
{
    if (!g_dpOn) return Real_DofRender(dof, fbo, depth, noise, depthScale);
    BYTE* r = g_dpR;
    int why = DofWhy(r, dof, fbo);
    if (why < 0 && DeadPassMode() == 2 && DpCheckDue(g_dpDof, 2)) {
        // Checked: the composite copied before and after the real pass, compared later.
        void* backend = *(void**)(dof + kDpDofBackend);
        DpBuf& a = g_dpBuf[0];
        DpBuf& b = g_dpBuf[1];
        if (!DpEnsure(a, backend, fbo, "TeardownVR::DeadPassCheck") || !DpEnsure(b, backend, fbo, "TeardownVR::DeadPassCheck")) {
            DpCheckFailed("its targets could not be made");
            why = kDpWhyCheck;
        } else {
            g_dpCopy(backend, a.fbo, fbo, 0, 0);
            Real_DofRender(dof, fbo, depth, noise, depthScale);
            g_dpCopy(backend, b.fbo, fbo, 0, 0);
            DpReadback(backend, a, true);
            DpReadback(backend, b, true);
            DpArm(2, backend, &a, &b, &a);
            ++g_dpDof.run;
            ++g_dpDof.why[kDpWhyCheck];
            return;
        }
    }
    if (why >= 0) {
        ++g_dpDof.run;
        ++g_dpDof.why[why];
        return Real_DofRender(dof, fbo, depth, noise, depthScale);
    }
    // Dead: nothing drawn. Its noise angle, as Dof::render writes it (+81136): frame number % 64, unless the game
    // is paused outside screenshot mode (DpCommon: not in screenshot mode here).
    const BYTE* game = *g_dpGame;
    if (!game[kDpGamePaused] || game[kDpGameShot]) *g_dpDofAngle = (float)(*(const int*)(game + kDpGameFrame) % 64);
    ++g_dpDof.skip;
    ++g_dpDof.skipAll;
    if (!g_dpDof.logged) {
        g_dpDof.logged = true;
        Log("Engine: skipDeadPasses: depth of field not drawn (amount exactly 0; its focus pass still runs)");
    }
}

// The post pass (renderer, the anti-aliased image). Only a checked bloom frame changes anything here: the post pass
// is drawn with the bloom result cleared (what a skip leaves), copied; the real bloom put back (an exact copy) and
// drawn again (the image shown, as without the check), copied; the two compared later.
void __fastcall Hook_PostPass(BYTE* r, void* input)
{
    auto& c = g_dpChk;
    if (!g_dpOn || !c.postArmed || c.postArmed != r || c.postFrame != g_frameCounter) {
        c.postArmed = nullptr;
        return Real_PostPass(r, input);
    }
    c.postArmed = nullptr;
    void* backend = *(void**)(r + kRendBackend);
    BYTE* bloom = *(BYTE**)(r + kDpBloom);
    BYTE* res = bloom ? *(BYTE**)(bloom + kDpBloomResult) : nullptr;
    BYTE* post = *(BYTE**)(r + kDpPostFbo);
    DpBuf& z = g_dpBuf[2];
    DpBuf& o = g_dpBuf[3];
    DpBuf& s = g_dpBuf[4];
    if (!res || !post || !DpEnsure(z, backend, post, "TeardownVR::DeadPassCheck") ||
        !DpEnsure(o, backend, post, "TeardownVR::DeadPassCheck") || !DpEnsure(s, backend, res, "TeardownVR::DeadPassCheck")) {
        DpCheckFailed("its targets could not be made");
        return Real_PostPass(r, input);
    }
    g_dpCopy(backend, s.fbo, res, 0, 0);                                // the real bloom, kept
    g_dpClear(backend, *(void**)(res + kDpFboTex), kDpClearColour);     // what a skip leaves instead
    Real_PostPass(r, input);
    g_dpCopy(backend, z.fbo, post, 0, 0);
    g_dpCopy(backend, res, s.fbo, 0, 0);                                // the real bloom back
    Real_PostPass(r, input);                                            // the original post pass: the image shown
    g_dpCopy(backend, o.fbo, post, 0, 0);
    DpReadback(backend, z, true);
    DpReadback(backend, o, true);
    DpReadback(backend, s, false);   // (the copy back left it in COPY_SOURCE)
    DpArm(1, backend, &z, &o, &s);
}

// ---------------------------------------------------------------------------
// The same noise in both eyes ([Affichage] stereoSameNoise)
// ---------------------------------------------------------------------------
// Teardown animates all its screen-space noise with one number per frame, FrameRnd: mubFrameParams.w of the scene
// constants (SceneDynamicBuffer +0x1AC; "r - Time, g - ScreenshotModeSimTime, b - SimTime, a - FrameRnd" in the
// game's shader source), blueNoise() = frac(blue noise texel + 0.618 * FrameRnd). It drives the dithered (stochastic)
// transparency of every particle - rain, smoke, fire, the extinguisher: a particle pixel is drawn opaque when that
// value is below its alpha, else discarded, and the TAA blends the frames - and of glass, and the sample offsets of
// AO / sun shadows, lamps, fog, reflections, water, wetness and their denoisers. The scene constants (+9e130, called
// at Renderer::render +75e) take FrameRnd from the TAA counter (TemporalAa +0: (n + 1) % 8 at TemporalAa::update,
// Renderer::render +37a, which also picks the frame's sub-pixel jitter and the TAA ping-pong from it), except while
// the game is paused (game+1E8h, not in photo mode: FrameRnd frozen). The per-eye history gives each eye its own
// counter, created one frame apart, each stepped once per pair: the right eye always drew the NEXT noise frame (at
// the same pixel, B's dither threshold window is A's moved by 0.618). With stereoSameNoise=1 frame B's render starts
// with the counter frame A's render started with, so TemporalAa::update gives frame B frame A's noise frame, jitter
// and ping-pong parity: the noise is identical at the same PIXEL of both eyes. Mode 0 gives frame B frame A's END
// counter: one noise frame ahead, exactly as before (also after a live 1 -> 0 switch, which would otherwise leave
// the right eye's own counter in phase). The left eye is unchanged, each eye still steps through the 8 noise frames
// / jitter offsets once per pair (TAA converges as before). Only for a frame B with its own TAA history
// (perEyeHistory, g_histActive == 1): with shared TAA textures A's parity would make B read the history B wrote one
// pair earlier, reprojected with frame A's (the other eye's) matrices. A live switch gives the right eye's TAA one
// frame of a one-pair-older history. LIMIT: what the eyes fuse is the same 3D point, not the same pixel. With
// offCenter=1 the two eyes' pixel grids are offset (W * (tanL0 - tanL1) / (tanL + tanR): 733 px on a Quest 3)
// and a point at depth z adds f * IPD / z px, so the screen-locked dither of rain, smoke and particles still differs
// between the eyes at the same point: this is the prerequisite of a stereo-consistent dither (the right eye's
// particle shader sampling the noise where the left eye sees the same point), not that dither itself. Checked after
// every render: the FrameRnd and the jitter index frame A and frame B drew with (log line every minute). Not
// covered: the depth of field's noise angle (game+1F0h % 64, Dof::render, only when it is drawn: removeDof=0) and
// photo mode's random jitter.
struct SameNoiseState
{
    bool ok = false;                  // TemporalAa::update and the scene constants recognised (InitSameNoise)
    const int* frameRnd = nullptr;    // the int the scene constants turn into FrameRnd (+9e130's global)
    int mode = -1;                    // the mode last used (logged when it changes)
    int counterA = 0;                 // the TAA counter at the start of frame A's (first) render
    uint64_t frameA = 0, frameB = 0;  // engine frame of that render / of the last frame B given it
    // the check: what frame A's first render drew with, then frame B's
    uint64_t checkA = 0, checkB = 0;
    int rndA = 0, idxA = 0, rndB = 0, idxB = 0;
    float jitA[2] = {}, jitB[2] = {};
    uint64_t pairs = 0, sameRnd = 0, sameIdx = 0, set = 0;
    ULONGLONG logAt = 0;
    bool sharedLogged = false;        // (the "frame B without its own TAA history" line, once)
};
SameNoiseState g_sameNoise;
constexpr uint32_t kTaaJitter = 0x18;   // TemporalAa +18h / +1Ch: the frame's sub-pixel jitter (TemporalAa::update)

// 0 off, 1 on: the control file TeardownVR_perf.txt ("stereoSameNoise=N") first.
int SameNoiseMode()
{
    const int f = FrameB::FileSameNoise();
    return std::clamp(f >= 0 ? f : Cfg().stereoSameNoise, 0, 1);
}

// Main thread, Hook_Render, before Renderer::render (after the per-eye history swap).
void SameNoiseBegin(BYTE* r)
{
    SameNoiseState& s = g_sameNoise;
    if (!s.ok || !r || !g_vrActive || !g_ready) return;
    BYTE* taa = *(BYTE**)(r + kRendTaa);
    if (!taa) return;
    const int mode = SameNoiseMode();
    if (mode != s.mode) {
        Log("Engine: stereoSameNoise=%d%s: %s", mode, FrameB::FileSameNoise() >= 0 ? " (control file)" : "",
            mode ? "the right eye draws with the left eye's noise frame and sub-pixel jitter (the same noise at the same "
                   "pixel of both eyes)"
                 : "off - the right eye draws the next noise frame and jitter (as before)");
        s.mode = mode;
        s.pairs = s.sameRnd = s.sameIdx = s.set = 0;
        s.logAt = GetTickCount64() + 5000;
    }
    if (!g_rightThisFrame) {
        if (s.frameA != g_frameCounter) {   // (the frame's first render)
            s.counterA = *(const int*)taa;
            s.frameA = g_frameCounter;
        }
        return;
    }
    // Frame B right after its frame A (not after a frame A without a render), its first render.
    if (!s.frameA || s.frameA + 1 != g_frameCounter || s.frameB == g_frameCounter) return;
    // Only with the right eye's own TAA history: on shared TAA textures frame A's ping-pong parity would make frame B
    // read the history it wrote one pair earlier while its reprojection matrices are frame A's (the other eye).
    if (!(Cfg().perEyeHistory && g_histActive == 1)) {
        if (!s.sharedLogged) {
            s.sharedLogged = true;
            Log("Engine: stereoSameNoise: the right eye has no TAA history of its own (perEyeHistory=0 or its buffers "
                "could not be made) - its noise frame is left as it is");
        }
        return;
    }
    s.frameB = g_frameCounter;
    if (mode == 1) {
        // TemporalAa::update makes of frame A's start counter what frame A drew with: same FrameRnd, jitter, parity.
        *(int*)taa = s.counterA;
        ++s.set;
    } else if (s.checkA == s.frameA) {
        // Mode 0: frame A's end counter, the old steady state exactly (the right eye one noise frame ahead), also
        // after mode 1 ran (which left the right eye's own counter in phase with the left eye's).
        *(int*)taa = s.idxA;
    }
}

// Main thread, Hook_Render, after Renderer::render: the noise frame and the jitter it drew with (the check, its log).
void SameNoiseEnd(BYTE* r)
{
    SameNoiseState& s = g_sameNoise;
    if (!s.ok || !r || !g_vrActive || !g_ready || s.mode < 0) return;
    BYTE* taa = *(BYTE**)(r + kRendTaa);
    if (!taa) return;
    const int rnd = *s.frameRnd, idx = *(const int*)taa;
    const float* jit = (const float*)(taa + kTaaJitter);
    if (!g_rightThisFrame) {
        if (s.checkA != g_frameCounter) {
            s.checkA = g_frameCounter;
            s.rndA = rnd;
            s.idxA = idx;
            s.jitA[0] = jit[0];
            s.jitA[1] = jit[1];
        }
        return;
    }
    if (!s.checkA || s.checkA + 1 != g_frameCounter || s.checkB == g_frameCounter) return;
    s.checkB = g_frameCounter;
    s.rndB = rnd;
    s.idxB = idx;
    s.jitB[0] = jit[0];
    s.jitB[1] = jit[1];
    ++s.pairs;
    if (s.rndB == s.rndA) ++s.sameRnd;
    if (s.idxB == s.idxA) ++s.sameIdx;
    const ULONGLONG now = GetTickCount64();
    if (now < s.logAt) return;
    s.logAt = now + 60000;
    Log("Engine: stereoSameNoise=%d: %llu pairs, the same noise frame in both eyes in %llu, the same jitter in %llu "
        "(frame B given frame A's TAA counter: %llu; last pair, left / right: noise frame %d / %d, jitter %d / %d = "
        "%+.6f %+.6f / %+.6f %+.6f)", s.mode, (unsigned long long)s.pairs, (unsigned long long)s.sameRnd,
        (unsigned long long)s.sameIdx, (unsigned long long)s.set, s.rndA, s.rndB, s.idxA, s.idxB, s.jitA[0],
        s.jitA[1], s.jitB[0], s.jitB[1]);
    s.pairs = s.sameRnd = s.sameIdx = s.set = 0;
}

void __fastcall Hook_Render(BYTE* r, void* a2, void* a3, void* a4)
{
    // perfTrace: our code before the render (P_Ours, the shape interpolation wait included).
    const double ours0 = g_perfTrace ? ProfNow() : 0;
    // pauseWide: the frame on either side of a 16:9 pause starts a fresh TAA history (the game's own reset flag,
    // as after a camera cut; with perEyeHistory, both eyes' histories below) - the anamorphic pause image is not
    // blended into the eye's view, nor the eye's view into it.
    if (g_wide.resetHistory) {
        g_wide.resetHistory = false;
        if (g_historyOk && r) r[kRendReset] = 1;
    }
    if (g_historyOk && Cfg().perEyeHistory && r) {
        int eye = g_rightThisFrame ? 1 : 0;
        ++g_renders;
        if (r[kRendReset]) g_histPending[0] = g_histPending[1] = true;   // the game asked for a history reset
        if (g_renders - g_lastRender[eye] > 2) g_histPending[eye] = true; // this eye's history is stale
        g_lastRender[eye] = g_renders;
        if (eye != g_histActive) {
            HistCapture(g_hist[g_histActive], r);
            if (HistEnsure(g_hist[eye], r, eye)) {
                HistApply(g_hist[eye], r);
                g_histActive = eye;
            }
        }
        if (g_histActive == eye && g_histPending[eye]) {
            r[kRendReset] = 1;
            g_histPending[eye] = false;
        }
    }
    // (stereoParticleMotion: whether this frame B has the right eye's own history - for the next frame B's decision)
    if (g_rightThisFrame) g_histRightOwn = g_historyOk && Cfg().perEyeHistory && r && g_histActive == 1;
    // ([Affichage] stereoSameNoise: frame B starts with frame A's TAA counter - the same noise frame and jitter)
    SameNoiseBegin(r);
    if (r && g_vrActive) TaaVrUpdate(*(BYTE**)(r + kRendTaa));
    // ([Affichage] stereoParticles: the VR or the game's particle pipelines; the scene constants learn which eye this
    // render draws - frame A, frame B of a stereo pair, or neither)
    ParticlesVrBeginRender(r, g_vrActive && g_ready ? (g_rightThisFrame ? 1 : 0) : -1, g_frameCounter);
    if (g_historyOk && g_vrActive && g_ready && (Cfg().perEyeHistory || Cfg().syncScriptMotion)) {
        // Motion vectors are rebuilt only for an eye that really has its own history (if its
        // buffers could not be created it uses the other eye's, i.e. the previous frame).
        const int eye = g_rightThisFrame ? 1 : 0;
        FixObjectMotion((BYTE*)a3, eye, Cfg().perEyeHistory && g_histActive == eye, Cfg().syncScriptMotion);
    }
    // skipShapeJobsB: frame B's shape interpolation jobs were not started and FixObjectMotion did not do their
    // work (it stopped before its right-eye pass): done here for every shape, as the jobs would have.
    if (g_shapeSkip.frame == g_frameCounter && !g_shapeSkip.done) ShapeJobsByHandAll();
    if (g_vrActive && g_ready) OffArmLateFix((BYTE*)a3, g_rightThisFrame ? 1 : 0);   // (after FixObjectMotion)
    // Fast particles (sparks, debris) are stretched along their motion by velocity x
    // frame time (game+0x204, read by the particle pass of this function). The
    // right-eye frame has (almost) no time, so its particles were not stretched: it
    // gets the left eye's frame time for the render only.
    float* dtField = nullptr;
    float savedDt = 0;
    if (g_rightThisFrame && g_vrActive && g_gameObj && g_offDt && g_lastLeftDt > 0 && Cfg().syncParticles) {
        dtField = (float*)(g_gameObj + g_offDt);
        savedDt = *dtField;
        *dtField = g_lastLeftDt;
    }
    if (g_perfTrace) g_prof[P_Ours] += ProfNow() - ours0;
    // ([Performance] perfDetail 8: pass boundaries in the game's command list; nothing when it is off)
    if (g_perfTrace && r) Perf::GpuPassMark(*(void**)(r + kRendBackend), 1);
    // ([Performance] skipDeadPasses: its log and check; the hooks of Bloom::render, Dof::render and the post pass
    // act only for the renderer drawn here)
    DeadPassPoll();
    BYTE* const dpPrev = g_dpR;
    g_dpR = r;
    {
        ProfScope prof(P_Render);
        Real_Render(r, a2, a3, a4);
    }
    g_dpR = dpPrev;
    SameNoiseEnd(r);
    ParticlesVrEndRender();
    if (g_perfTrace && r) Perf::GpuPassMark(*(void**)(r + kRendBackend), 2);
    if (dtField) *dtField = savedDt;
}

// ---------------------------------------------------------------------------
// Particles: the depth sort of a frame without a physics step, on three threads
// ---------------------------------------------------------------------------
// ([Performance] parallelParticleSort.) Each particle system (three in 2.0.1, types 0 to 2) is sorted
// once per rendered frame by 0x437df0: its particles culled against the player's camera (game+0x60)
// with the quality's level-of-detail stride, sorted by depth (a radix sort, 0x4320d0) and its mesh
// buffers sized. In a frame with a physics step it runs at the end of each system's
// PostSimulationJob, on the job workers: the three systems at the same time. In a frame without one
// (every right-eye frame B, and the left-eye frames whose physics loop ran no step)
// ParticleSystem::update calls it on the main thread for one system after the other (0x42f510)
// before it creates the mesh jobs: 0.3-0.4 ms of frame B after explosions. The function reads only
// its own system, the camera and the quality, writes only its own system (its sorted list
// +0x1ac0178, its scratch list +0x1b40180, its mesh buffer sizes +0xa801f8 / +0xa80228), reads no
// thread-local data and takes its memory from the CRT heap. So the calls of that loop are made at
// once here, as the engine makes them in step frames: the largest system on the main thread, the
// next ones on two threads of ours, and the loop's first call returns once all are done, before
// the loop creates any mesh job (each reads its own system's list). Same function, same inputs,
// separate outputs: the lists and meshes are the serial loop's. parallelParticleSort=2 checks it:
// each system sorted in parallel is sorted again on the main thread and the two results compared.
using PFN_PsSort = void(__fastcall*)(BYTE* system);
PFN_PsSort Real_PsSort = nullptr;
BYTE* g_psSortRet = nullptr;   // return address of the sort's call in the no-step loop (+42f57d)
BYTE* g_psCur = nullptr;       // the systems' owner while ParticleSystem::update runs (Hook_PsUpdate)
DWORD g_psTid = 0;             // (its thread: the main thread)
uint32_t g_psSortDone = 0;     // systems of this update sorted already (bit = index in the owner's list)
bool g_psSortDecided = false;  // this update's sort decided (at its first call): the other calls go straight on
bool g_psSortBroken = false;   // a helper's exception or a check's difference: one after the other again
constexpr uint32_t kPsCount = 0x00, kPsSystems = 0x08;   // owner: { int count; System** (+8) }
constexpr uint32_t kSysSorted = 0x1ac0178;               // system: { int count; int capacity; {float, int}* (+8) }
constexpr uint32_t kSysMeshSizes[3] = { 0xa80218, 0xa80220, 0xa80230 };   // its mesh buffer sizes
constexpr int kSortHelpers = 2;
// Particles the helpers would take, at least: below that a helper's wake-up (an event set, 10-30 us)
// costs about what it saves.
constexpr int kSortMinMoved = 1000;

// A helper's system: given (kQueued), then taken either by the helper (kRunning, back to kIdle when
// sorted) or, when the main thread is done with its own one first, by the main thread (kTakenBack):
// a helper woken late (a busy core) never makes the main thread wait longer than sorting it itself.
// The events only wake: the state says what to do.
constexpr int kSortIdle = 0, kSortQueued = 1, kSortRunning = 2, kSortTakenBack = 3;
struct SortHelper
{
    HANDLE go = nullptr, done = nullptr;   // auto-reset events
    BYTE* system = nullptr;                // (written before the state goes to kSortQueued)
    unsigned mxcsr = 0;                    // the main thread's SSE control word (same float results)
    std::atomic<int> state{ kSortIdle };
    bool fault = false;                    // (written before the state goes back to kSortIdle)
    double ms = 0;
};
SortHelper g_sortHelper[kSortHelpers];
int g_sortHelpers = -1;                    // threads running (-1: not started yet)
struct SortStats
{
    uint64_t parallel = 0, serial = 0, checked = 0, diffs = 0, takenBack = 0;
    uint64_t logParallel = 0, logSerial = 0;   // (since the last summary line)
    double wallMs = 0, sumMs = 0;              // parallel frames since then: the main thread's time, the sorts' total
    bool logged = false;
} g_sortStats;

// 0 off, 1 on, 2 on and checked: the control file TeardownVR_perf.txt ("parallelParticleSort=N") first.
int ParallelSortMode()
{
    const int f = FrameB::FileParticleSort();
    return std::clamp(f >= 0 ? f : Cfg().parallelParticleSort, 0, 2);
}

// The engine's sort under SEH: an exception must not end the game on a helper thread (the main thread
// then sorts that system again itself, as the engine would have).
bool SortGuarded(BYTE* system)
{
    __try {
        Real_PsSort(system);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI SortHelperThread(void* arg)
{
    SortHelper& h = *(SortHelper*)arg;
    for (;;) {
        WaitForSingleObject(h.go, INFINITE);
        int queued = kSortQueued;
        if (!h.state.compare_exchange_strong(queued, kSortRunning, std::memory_order_acquire)) continue;   // (taken back)
        _mm_setcsr(h.mxcsr);
        const double t0 = ProfNow();
        h.fault = !SortGuarded(h.system);
        h.ms = ProfNow() - t0;
        h.state.store(kSortIdle, std::memory_order_release);
        SetEvent(h.done);
    }
}

// Two threads at the main thread's priority (the sort is on its path), started at the first use.
bool StartSortHelpers()
{
    if (g_sortHelpers >= 0) return g_sortHelpers > 0;
    g_sortHelpers = 0;
    const int prio = GetThreadPriority(GetCurrentThread());
    for (SortHelper& h : g_sortHelper) {
        h.go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        h.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = h.go && h.done ? CreateThread(nullptr, 256 * 1024, SortHelperThread, &h, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                                 nullptr)
                                  : nullptr;
        if (!t) break;
        SetThreadPriority(t, prio);
        CloseHandle(t);
        ++g_sortHelpers;
    }
    if (g_sortHelpers < kSortHelpers)
        Log("Engine: parallel particle sort: %d helper thread(s) of %d started", g_sortHelpers, kSortHelpers);
    return g_sortHelpers > 0;
}

// Until the helper is done with the system it took: a short spin first (its system is a smaller one:
// usually done by then), then its event (a signal left over from an earlier spin only costs one more
// look).
void WaitSortHelper(SortHelper& h)
{
    const double until = ProfNow() + 0.1;
    for (int k = 1; h.state.load(std::memory_order_acquire) == kSortRunning; ++k) {
        if (k % 64 == 0 && ProfNow() > until) {
            while (h.state.load(std::memory_order_acquire) == kSortRunning) WaitForSingleObject(h.done, INFINITE);
            return;
        }
        _mm_pause();
    }
}

// A system's sort result (parallelParticleSort=2): its sorted list's length and entries, its mesh sizes.
void SortResult(BYTE* system, std::vector<uint64_t>& out)
{
    out.clear();
    const int n = *(const int*)(system + kSysSorted);
    const uint64_t* items = *(const uint64_t* const*)(system + kSysSorted + 8);
    out.push_back((uint32_t)n);
    for (uint32_t off : kSysMeshSizes) out.push_back(*(const uint32_t*)(system + off));
    if (n > 0 && items) out.insert(out.end(), items, items + n);
}

// parallelParticleSort=2: each system just sorted at once, sorted again here (one after the other, the
// engine's way) and compared. A difference: one after the other from now on (the second result stays).
void SortCheck(BYTE** list, const int* order, int count)
{
    static std::vector<uint64_t> a, b;
    for (int k = 0; k < count; ++k) {
        BYTE* sys = list[order[k]];
        SortResult(sys, a);
        Real_PsSort(sys);
        SortResult(sys, b);
        ++g_sortStats.checked;
        if (a == b) continue;
        ++g_sortStats.diffs;
        g_psSortBroken = true;
        Log("Engine: parallel particle sort CHECK: system %d differs from the serial sort (%llu / %llu values) - one "
            "after the other from now on", order[k], (unsigned long long)a.size(), (unsigned long long)b.size());
    }
}

void __fastcall Hook_PsSort(BYTE* system)
{
    // Only the main thread's calls in the no-step loop (the workers' ones at the end of a simulation job
    // come through here too, and go straight on).
    if (_ReturnAddress() != g_psSortRet || !g_psCur || GetCurrentThreadId() != g_psTid) return Real_PsSort(system);
    const int n = std::min(*(const int*)(g_psCur + kPsCount), 32);
    BYTE** list = *(BYTE***)(g_psCur + kPsSystems);
    int self = -1;
    for (int i = 0; list && i < n; ++i)
        if (list[i] == system) {
            self = i;
            break;
        }
    if (self < 0) return Real_PsSort(system);
    if (g_psSortDone & (1u << self)) return;   // sorted with an earlier system of this loop
    if (g_psSortDecided) return Real_PsSort(system);   // (left to the loop by its first call)
    g_psSortDecided = true;
    const int mode = ParallelSortMode();
    if (!mode || g_psSortBroken || !StartSortHelpers()) return Real_PsSort(system);
    // This system and the ones after it in the loop (its first call: all of them), largest first: the
    // first on this thread, the next non-empty ones on the helpers; the others stay with the loop.
    auto count = [&](int i) { return *(const int*)list[i]; };
    int order[32], m = 0;
    for (int i = self; i < n; ++i) order[m++] = i;
    std::stable_sort(order, order + m, [&](int a, int b) { return count(a) > count(b); });
    int helpers = 0, moved = 0;
    for (int k = 1; k < m && helpers < g_sortHelpers && count(order[k]) > 0; ++k, ++helpers) moved += count(order[k]);
    if (!helpers || moved < kSortMinMoved) {
        ++g_sortStats.serial;
        ++g_sortStats.logSerial;
        return Real_PsSort(system);
    }
    const double t0 = ProfNow();
    const unsigned csr = _mm_getcsr();
    for (int k = 0; k < helpers; ++k) {
        SortHelper& h = g_sortHelper[k];
        h.system = list[order[1 + k]];
        h.mxcsr = csr;
        h.fault = false;
        h.ms = 0;
        h.state.store(kSortQueued, std::memory_order_release);
        SetEvent(h.go);
    }
    Real_PsSort(list[order[0]]);
    double sum = ProfNow() - t0;
    for (int k = 0; k < helpers; ++k) {
        SortHelper& h = g_sortHelper[k];
        int queued = kSortQueued;
        if (h.state.compare_exchange_strong(queued, kSortTakenBack, std::memory_order_acq_rel)) {
            // (its helper has not started it yet: sorted here, as the loop would have)
            const double t1 = ProfNow();
            Real_PsSort(h.system);
            sum += ProfNow() - t1;
            h.state.store(kSortIdle, std::memory_order_release);
            ++g_sortStats.takenBack;
            continue;
        }
        WaitSortHelper(h);
        sum += h.ms;
        if (h.fault) {
            // (sorted again here, as the engine would have - and if it fails again, as it would have)
            if (!g_psSortBroken) Log("Engine: parallel particle sort: exception on a helper thread - one system after the other from now on");
            g_psSortBroken = true;
            Real_PsSort(h.system);
        }
    }
    const double wall = ProfNow() - t0;
    for (int k = 0; k <= helpers; ++k) g_psSortDone |= 1u << order[k];
    if (mode == 2) SortCheck(list, order, helpers + 1);
    if (!(g_psSortDone & (1u << self))) Real_PsSort(system);   // (an empty one left to the loop: this is its call)
    ++g_sortStats.parallel;
    ++g_sortStats.logParallel;
    g_sortStats.wallMs += wall;
    g_sortStats.sumMs += sum;
    if (!g_sortStats.logged) {
        g_sortStats.logged = true;
        Log("Engine: particle depth sort on %d threads in the frames without a physics step (parallelParticleSort=%d): "
            "%d particles here, %d on the helpers, %.3f ms for %.3f ms of sorting", helpers + 1, mode, count(order[0]),
            moved, wall, sum);
    }
    if (g_sortStats.logParallel + g_sortStats.logSerial >= 6000) {
        char chk[96] = "";
        if (g_sortStats.checked)
            snprintf(chk, sizeof(chk), "; checked %llu systems: %llu different", (unsigned long long)g_sortStats.checked,
                     (unsigned long long)g_sortStats.diffs);
        Log("Engine: parallel particle sort: %llu frames without a step sorted at once, %llu one after the other (under %d "
            "particles to share); at once: main thread %.3f ms per frame for %.3f ms of sorting%s; %llu systems taken back "
            "from a helper not started yet (in all)",
            (unsigned long long)g_sortStats.logParallel, (unsigned long long)g_sortStats.logSerial, kSortMinMoved,
            g_sortStats.logParallel ? g_sortStats.wallMs / g_sortStats.logParallel : 0.0,
            g_sortStats.logParallel ? g_sortStats.sumMs / g_sortStats.logParallel : 0.0, chk,
            (unsigned long long)g_sortStats.takenBack);
        g_sortStats.logParallel = g_sortStats.logSerial = 0;
        g_sortStats.wallMs = g_sortStats.sumMs = 0;
    }
}

// Particles. ParticleSystem::update runs once per frame in the draw, after the
// physics loop. When a frame is shorter than 0.8 x the physics step (above 75 fps)
// it uses the step counter and alpha saved by the PREVIOUS frame (a "late" mode that
// spreads the load on a desktop). In synchronised stereo every frame is that short,
// so the particles of each physics step were simulated in the zero-time right-eye
// frame: the left eye showed smoke and sparks one display interval behind the right
// eye (about 10 cm for fast particles) and the right-eye frame paid for the whole
// simulation. Writing the current values into the "previous" slots makes both paths
// identical: the left-eye frame simulates its own step, the right-eye frame (no new
// step) only rebuilds the particle mesh from the same state.
// Found by reverse engineering (docs/REVERSE_ENGINEERING.md, section 6 quinquies).
using PFN_PsUpdate = void(__fastcall*)(BYTE* ps, void* a2, void* a3, void* a4);
PFN_PsUpdate Real_PsUpdate = nullptr;
uint32_t g_psCurTime = 0, g_psPrevTime = 0, g_psPrevAlpha = 0, g_psPrevStep = 0, g_gameAlpha = 0, g_gameStep = 0;

namespace {
void ParticleMotionDecide(BYTE* ps);   // ([Affichage] stereoParticleMotion, with its mesh task hook below InitParticleSort)
}

void __fastcall Hook_PsUpdate(BYTE* ps, void* a2, void* a3, void* a4)
{
    double p0 = 0;
    if (g_perfTrace) {
        // perfTrace: Game::update's exit to here = the fixed-step loop (+ the start of the draw);
        // the step counter tells how many physics steps it ran.
        p0 = ProfNow();
        if (g_perfUpdExit > 0) {
            g_prof[P_FixedLoop] += p0 - g_perfUpdExit;
            if (g_gameObj && g_perfStepAtExit != INT_MIN)
                Perf::NoteSteps(*(const int*)(g_gameObj + g_gameStep) - g_perfStepAtExit);
            g_perfUpdExit = 0;
        }
    }
    if (ps && g_gameObj && g_vrActive && Cfg().syncParticles) {
        *(float*)(ps + g_psPrevTime) = *(float*)(ps + g_psCurTime);
        *(float*)(ps + g_psPrevAlpha) = *(float*)(g_gameObj + g_gameAlpha);
        *(int*)(ps + g_psPrevStep) = *(int*)(g_gameObj + g_gameStep);
    }
    // parallelParticleSort: the systems' owner for the depth sort of a frame without a physics step
    // (Hook_PsSort; main thread, during this call only).
    g_psCur = ps && g_vrActive && g_ready && Real_PsSort ? ps : nullptr;
    g_psTid = GetCurrentThreadId();
    g_psSortDone = 0;
    g_psSortDecided = false;
    // stereoParticleMotion: what the particle mesh tasks this update creates do (Hook_MeshTask) - decided here, on the
    // main thread, before any of them exists. It is not taken back when the update returns: in a frame with a physics
    // step the tasks are started at the end of each system's PostSimulationJob, on the workers, and may still run.
    ParticleMotionDecide(ps);
    Real_PsUpdate(ps, a2, a3, a4);
    g_psCur = nullptr;
    if (g_perfTrace) g_prof[P_Particles] += ProfNow() - p0;
}

void InitParticleHook()
{
    // Scene::preRender: mov rcx,[r13+38h] ; call FireSystem::lights ; mov rcx,[r13+18h] ; call ParticleSystem::update
    BYTE* site = Scan("49 8B 4D 38 E8 ?? ?? ?? ?? 49 8B 4D 18 E8 ?? ?? ?? ??");
    BYTE* fn = site ? RelTarget(site + 18, site + 14) : nullptr;
    // movss xmm6,[r13+cur] ; mov rax,[g_game] ; movss xmm7,[rax+alpha] ; mov esi,[rax+step]
    BYTE* cur = fn ? ScanIn(fn, 0x140, "F3 41 0F 10 B5 ?? ?? 00 00 48 8B 05 ?? ?? ?? ?? F3 0F 10 B8 ?? ?? 00 00 8B B0 ?? ?? 00 00") : nullptr;
    // late path: movss xmm6,[r13+prevTime] ; movss xmm7,[r13+prevAlpha] ; mov esi,[r13+prevStep]
    BYTE* late = cur ? ScanIn(cur + 30, 0x60, "F3 41 0F 10 B5 ?? ?? 00 00 F3 41 0F 10 BD ?? ?? 00 00 41 8B B5 ?? ?? 00 00") : nullptr;
    if (!late) {
        Log("Engine: particle update not recognised (site %p fn %p) - particles stay one frame late in the left eye", site, fn);
        return;
    }
    g_psCurTime = U32(cur + 5);
    g_gameAlpha = U32(cur + 20);
    g_gameStep = U32(cur + 26);
    g_psPrevTime = U32(late + 5);
    g_psPrevAlpha = U32(late + 14);
    g_psPrevStep = U32(late + 21);
    if (g_psPrevTime != g_psCurTime + 4 || g_psPrevAlpha != g_psCurTime + 8 || g_psPrevStep != g_psCurTime + 12) {
        Log("Engine: particle update fields unexpected (%x %x %x %x) - left as is", g_psCurTime, g_psPrevTime,
            g_psPrevAlpha, g_psPrevStep);
        return;
    }
    if (!HookFunction(fn, (void*)Hook_PsUpdate, (void**)&Real_PsUpdate)) return;
    Log("Engine: particles synchronised with the physics step (update +%llx, fields %x/%x/%x/%x, game %x/%x)",
        (unsigned long long)(fn - g_base), g_psCurTime, g_psPrevTime, g_psPrevAlpha, g_psPrevStep, g_gameAlpha, g_gameStep);
}

bool Matches(BYTE* p, const char* pattern)
{
    return ScanIn(p, 64, pattern) == p;
}

namespace {
BYTE* ScanOnce(const char* pattern);   // (with the measurement's patterns, below)
}

// parallelParticleSort: the sort's call in ParticleSystem::update's no-step loop (0x42f510), found by the pattern
// below (the systems: count at the owner's +0, list at +8), and the sort's own start (its prologue, then the
// sorted count's address at +1AC0178h).
void InitParticleSort()
{
    if (!Real_PsUpdate) return;   // (its update hook gives the systems)
    if (!Cfg().parallelParticleSort && !FrameB::HooksWanted()) return;   // (off, and no control file can turn it on)
    BYTE* site = ScanOnce("8B DE 39 1F 76 ?? 66 90 48 63 C3 4C 8D 3C C5 00 00 00 00 48 8B 4F 08 49 8B 0C 0F 4C 3B F5 75 ?? "
                          "E8 ?? ?? ?? ?? 48 8B 47 08 49 8B 0C 07 48 89 74 24 60 4C 8D 44 24 60 48 8D 57 40 E8");
    BYTE* fn = site ? RelTarget(site + 37, site + 33) : nullptr;
    if (!fn || !Matches(fn, "48 8B C4 53 56 57 41 57 48 81 EC F8 00 00 00 48 89 68 10 48 8D 99 78 01 AC 01")) {
        Log("Engine: particle sort not recognised (site %p, sort %p) - one system after the other, as before", site, fn);
        return;
    }
    g_psSortRet = site + 37;
    // (MinHook reports a hook another design already put there as a success without the original)
    if (!HookFunction(fn, (void*)Hook_PsSort, (void**)&Real_PsSort) || !Real_PsSort) {
        Log("Engine: particle sort not hooked (MinHook failed, or another hook is there) - one system after the "
            "other, as before");
        return;
    }
    Log("Engine: parallel particle sort ready (sort +%llx, no-step loop +%llx; parallelParticleSort=%d)",
        (unsigned long long)(fn - g_base), (unsigned long long)(site - g_base), ParallelSortMode());
}

// ---------------------------------------------------------------------------
// Particles: the right eye's own particle motion ([Affichage] stereoParticleMotion)
// ---------------------------------------------------------------------------
// The particle meshes are built by MeshGenerationTask::run (+434160; its only reference is slot 1 of the tasks'
// vtable, .rdata +a291e0, used by both task creation sites +4328f3 / +432edf): ParticleSystem::update creates the
// tasks, at the end of each system's PostSimulationJob on the job workers in a frame with a physics step, after the
// main thread's sort otherwise. A task draws one range [task+48h, task+4Ch) of its system's (task+40h) depth-sorted
// list ({float view depth; int slot}: count at sys+1AC0178h, list at +1AC0180h; the particle at [sys+8] + slot x 0A8h)
// with the generator of the system's mesh mode (sys+1B40220h: 0 the CPU one +434830, 1 the GPU one +4341d0, other: no
// mesh). For each particle the generator takes the position it is drawn at now (+0 and +74h interpolated with the
// system's alpha, sys+1B40218h), puts the particle's last drawn position (+0Ch, 12 bytes) in the mesh as its "last
// position" (+4344f0, +434d29), then overwrites +0Ch with the position drawn now - in every iteration (+43477c,
// +434d38). The particle vertex shader's motion vector is the position now minus that one, through the previous
// frame's camera (mubOldStableVpMatrix; the stretch of fast sparks uses the velocity, not this): the TAA fetches the
// particle's history where it was and blends and clamps it for its motion, and the light denoiser and the
// disocclusion mask follow it.
// In synchronised stereo frame B (the right eye) builds its mesh from the same particle state as frame A. It read
// what frame A had just written in +0Ch, the position drawn now: the right eye's particles had no motion of their own
// (only the camera's - its TAA fetched their history where the background was, and blended and clamped moving smoke
// as if still: it trailed, smoothed differently from the left eye), while frame A, which reads frame B's write of the
// previous pair, has the left eye's motion right. So in frame A each task first notes, for each particle of its range,
// its +0Ch - the position the previous frame B drew, the right eye's previous position - in a table of ours (16 bytes
// per slot: the value, and the pair it was noted in); frame A only reads the game's memory. In frame B each task first
// puts that value back for the particles noted in this pair: frame B's generator then computes the right eye's own
// motion, and its unconditional overwrite leaves +0Ch exactly as without the fix - the next frame A reads the same
// values, and the left eye is unchanged, bit for bit. A particle born this pair (+0Ch still 0: never drawn; frame A
// gives it a motion from the origin) keeps no motion in frame B, as before. Only when nothing can have changed a
// particle between the two frames: frame A of the same pair noted it, the physics step counter has not moved (no
// simulation in frame B - syncParticles - so no slot died or was reused), and the right eye has its own TAA history
// (perEyeHistory, and the previous frame B really rendered with its buffers - g_histRightOwn; if HistEnsure fails, that
// frame B uses frame A's history): with a history shared by both eyes, zero motion in frame B is the right answer. Frame
// B's alpha is NOT compared with frame A's: it differs by a few ulps in many pairs (frame B takes its time back out of
// the accumulators in floats), far below a micron of particle position. A frame B skipped: the next one gets one pair
// of motion back instead of two (partial, never worse than before). Cost: one 16-byte read of our table and one
// 12-byte write per particle drawn in frame B, on the workers; 3 MB of tables. [Debug] particleMotionLog (or the
// control file's "particleMotionLog=1", or debugLua): counters once a minute and at each mode change, reads only -
// with stereoParticleMotion=0 they measure the motion frame B loses, without changing anything.
using PFN_MeshTask = void(__fastcall*)(BYTE* task, void* a2, void* a3, void* a4);

namespace {

PFN_MeshTask Real_MeshTask = nullptr;
constexpr uint32_t kTaskSystem = 0x40, kTaskBegin = 0x48, kTaskEnd = 0x4c;   // task -> system, its range of the list
// system -> its particles { int count (+0); ...; Particle* (+8) } (the sort, 0x437df0, lists slots 0..count-1)
constexpr uint32_t kSysParticles = 0x08;
constexpr uint32_t kSysMeshMode = 0x1b40220;   // system -> mesh mode (0: CPU generator, 1: GPU generator, other: none)
constexpr size_t kParticleSize = 0xa8;
constexpr uint32_t kPartDrawn = 0x0c;          // particle -> last drawn position (3 floats)
struct PmSorted { float depth; int slot; };    // a sorted list entry: view depth of the particle's +0, its slot
struct PmEntry { uint32_t p[3]; uint32_t pair; };   // a slot's +0Ch as frame A found it (bits), and that frame's number
constexpr int kPmSystems = 3;                  // (2.0.1: three particle systems, types 0 to 2)
constexpr uint32_t kPmSlots = 65536;           // slots per system in the table (beyond: as before)
PmEntry* g_pmTab = nullptr;                    // [kPmSystems][kPmSlots], allocated at the first use (main thread)
std::atomic<BYTE*> g_pmSys[kPmSystems];        // the owner's systems (Hook_PsUpdate, main thread)
// What this frame's mesh tasks do: engine frame << 8 | counters on | frame B | phase (Hook_PsUpdate, main thread;
// kept until its next call).
constexpr int kPmNone = 0, kPmRecord = 1, kPmRestore = 2, kPmMeasure = 3;
constexpr uint64_t kPmEyeB = 0x10, kPmLog = 0x20;
std::atomic<uint64_t> g_pmState{ 0 };
std::atomic<bool> g_pmBroken{ false };         // an exception in a task: off until the game restarts

// particleMotionLog: the tasks' counts (worker threads: each task adds its own once, at its end) and the histogram of
// the motion frame B had lost, for the particles that moved (distance / view depth: 4 bins per octave from 2^-24).
enum { kPmTasks, kPmOther, kPmNoted, kPmNotedNew, kPmChecked, kPmMoved, kPmGiven, kPmKeptNew, kPmNoNote, kPmBeyond,
       kPmWritesA, kPmCounts };
constexpr int kPmBins = 112, kPmBinLo = 24;
std::atomic<uint64_t> g_pmCount[kPmCounts];
std::atomic<uint64_t> g_pmHist[kPmBins];
struct PmTaskCount { uint32_t n[kPmCounts]; uint32_t hist[kPmBins]; };

// Main thread (Hook_PsUpdate).
enum { kPmWhyBroken, kPmWhySync, kPmWhyTable, kPmWhyNoPair, kPmWhyNoA, kPmWhyStep, kPmWhyHistory, kPmWhyN };
const char* const kPmWhyName[kPmWhyN] = { "off after an exception", "not synchronised (VR off, syncParticles off...)",
                                          "no memory for the table", "frame A of no stereo pair",
                                          "frame B without its frame A's notes", "frame B after a physics step",
                                          "no TAA history of the right eye's own (last frame B)" };
struct PmMain
{
    uint32_t pairA = 0;          // the last frame A whose tasks noted (its number, low 32 bits; 0 = none)
    int stepA = INT_MIN;         // the physics step counter then
    uint32_t alphaA = 0;         // the game's alpha then (bits; the counters)
    int loggedMode = -1;         // the mode of the last mode line
    int logMode = -1;            // the mode of the counters since the last counters line
    bool tableFailed = false, firstGiven = false, brokenLogged = false, logOn = false;
    ULONGLONG logStart = 0, logAt = 0;
    // (counters on: since the last counters line)
    uint64_t calls = 0, framesB = 0, phases[4] = {}, why[kPmWhyN] = {}, alphaPairs = 0, alphaDiff = 0;
    float alphaMax = 0;
};
PmMain g_pm;

// 0 = the game's (frame B: no particle motion of its own), 1 = frame B gets the right eye's previous positions back:
// the control file TeardownVR_perf.txt ("stereoParticleMotion=N") first. (Higher values read as 1.)
int ParticleMotionMode()
{
    const int f = FrameB::FileParticleMotion();
    return std::clamp(f >= 0 ? f : Cfg().stereoParticleMotion, 0, 1);
}

// The counters in the log (measuring only: they read, and write nothing of the game's).
bool ParticleMotionLogOn()
{
    return Cfg().particleMotionLog || FrameB::FileParticleMotionLog() == 1 || Cfg().debugLua;
}

int PmBin(float a)
{
    uint32_t u;
    memcpy(&u, &a, sizeof(u));
    const int k = ((int)((u >> 23) & 0xff) - 127 + kPmBinLo) * 4 + (int)((u >> 21) & 3);
    return std::clamp(k, 0, kPmBins - 1);
}

float PmBinValue(int k) { return std::ldexp(1.0f + ((k & 3) + 0.5f) / 4.0f, k / 4 - kPmBinLo); }

// One task's work, before the generator runs (worker thread): its range of the sorted list, walked as the generator
// walks it (clamped to the list's count, unsigned). c: the counters (null: off).
void PmTask(BYTE* task, uint64_t st, PmTaskCount* c)
{
    BYTE* sys = *(BYTE**)(task + kTaskSystem);
    int k = -1;
    for (int i = 0; sys && i < kPmSystems; ++i)
        if (g_pmSys[i].load(std::memory_order_relaxed) == sys) {
            k = i;
            break;
        }
    if (k < 0) {
        if (c) ++c->n[kPmOther];
        return;
    }
    // (the dispatcher's own test: another mode builds no mesh - nothing to note, and nothing may be changed, since no
    // generator would overwrite it)
    const int meshMode = *(const int*)(sys + kSysMeshMode);
    if (meshMode != 0 && meshMode != 1) return;
    const uint32_t n = *(const uint32_t*)(sys + kSysSorted);
    const uint32_t b = std::min(*(const uint32_t*)(task + kTaskBegin), n);
    const uint32_t e = std::min(*(const uint32_t*)(task + kTaskEnd), n);
    const PmSorted* list = *(const PmSorted* const*)(sys + kSysSorted + 8);
    BYTE* parts = *(BYTE**)(sys + kSysParticles);
    const int count = *(const int*)sys;
    if (b >= e || !list || !parts || count <= 0 || !g_pmTab) return;
    if (c) ++c->n[kPmTasks];
    const uint32_t bound = std::min((uint32_t)count, kPmSlots);
    PmEntry* tab = g_pmTab + (size_t)k * kPmSlots;
    const int phase = (int)(st & 0xf);
    const uint64_t frame = st >> 8;
    if (phase == kPmRecord) {
        const uint32_t pair = (uint32_t)frame;
        for (uint32_t i = b; i < e; ++i) {
            const int slot = list[i].slot;
            if (slot < 0 || (uint32_t)slot >= bound) {
                if (c) ++c->n[kPmBeyond];
                continue;
            }
            PmEntry& E = tab[slot];
            if (E.pair == pair) continue;   // (a second mesh of this frame A: the first one's note stays)
            memcpy(E.p, parts + (size_t)slot * kParticleSize + kPartDrawn, sizeof(E.p));
            E.pair = pair;
            if (c) {
                ++c->n[kPmNoted];
                if (!(E.p[0] | E.p[1] | E.p[2])) ++c->n[kPmNotedNew];
            }
        }
        return;
    }
    // Frame B (kPmRestore / kPmMeasure): the notes of frame A, the previous engine frame.
    const uint32_t pair = (uint32_t)(frame - 1);
    const bool give = phase == kPmRestore, eyeB = (st & kPmEyeB) != 0;
    for (uint32_t i = b; i < e; ++i) {
        const int slot = list[i].slot;
        if (slot < 0 || (uint32_t)slot >= bound) {
            if (c) ++c->n[kPmBeyond];
            continue;
        }
        const PmEntry& E = tab[slot];
        if (E.pair != pair) {   // (not drawn in frame A, or born since: as before)
            if (c) ++c->n[kPmNoNote];
            continue;
        }
        if (!(E.p[0] | E.p[1] | E.p[2])) {   // (born this pair: no position drawn before frame A - no motion, as before)
            if (c) ++c->n[kPmKeptNew];
            continue;
        }
        BYTE* drawn = parts + (size_t)slot * kParticleSize + kPartDrawn;
        if (c) {
            // What frame B loses without the fix: frame A's drawn position (here now) minus the noted one.
            float now[3], was[3];
            memcpy(now, drawn, sizeof(now));
            memcpy(was, E.p, sizeof(was));
            const float dx = now[0] - was[0], dy = now[1] - was[1], dz = now[2] - was[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            ++c->n[kPmChecked];
            if (d2 > 1e-10f) {   // (more than 0.01 mm)
                ++c->n[kPmMoved];
                ++c->hist[PmBin(std::sqrt(d2) / std::max(list[i].depth, 1e-3f))];
            }
        }
        if (give) {
            memcpy(drawn, E.p, sizeof(E.p));
            if (c) {
                ++c->n[kPmGiven];
                if (!eyeB) ++c->n[kPmWritesA];   // (never: frame A only reads)
            }
        }
    }
}

// Under SEH: a wrong offset in some later state of the game must not end it on a worker (the fix turns off).
bool PmTaskGuarded(BYTE* task, uint64_t st, PmTaskCount* c)
{
    __try {
        PmTask(task, st, c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PmAdd(const PmTaskCount& c)
{
    for (int i = 0; i < kPmCounts; ++i)
        if (c.n[i]) g_pmCount[i].fetch_add(c.n[i], std::memory_order_relaxed);
    for (int i = 0; i < kPmBins; ++i)
        if (c.hist[i]) g_pmHist[i].fetch_add(c.hist[i], std::memory_order_relaxed);
}

// MeshGenerationTask::run (the job system's threads). Nothing of ours unless Hook_PsUpdate decided a phase for this
// very frame and nothing went wrong so far; the generator always runs after, once, as without the hook.
void __fastcall Hook_MeshTask(BYTE* task, void* a2, void* a3, void* a4)
{
    const uint64_t st = g_pmState.load(std::memory_order_acquire);
    if ((st & 0xf) != kPmNone && task && (st >> 8) == g_pmFrameNow.load(std::memory_order_relaxed) &&
        !g_pmBroken.load(std::memory_order_relaxed)) {
        bool ok;
        if (st & kPmLog) {
            PmTaskCount c = {};
            ok = PmTaskGuarded(task, st, &c);
            if (ok) PmAdd(c);
        } else {
            ok = PmTaskGuarded(task, st, nullptr);
        }
        if (!ok) g_pmBroken.store(true, std::memory_order_relaxed);
    }
    Real_MeshTask(task, a2, a3, a4);
}

// The right eye's focal length in pixels, for the log's px: the game's width over its view's horizontal tangents
// (off-centre projection), else a Quest 3's (1363.4 px at 3024 px wide).
float PmFocalPx(int& width)
{
    const float* t = g_eyeTan[1];
    width = g_gameObj ? *(const int*)g_gameObj : 0;
    if (g_offCenter && width > 0 && width < 65536 && t[0] + t[1] > 0.01f) return (float)width / (t[0] + t[1]);
    width = 0;
    return 1363.4f;
}

// A new window of counters (the tasks' ones too: what they added while the counters were off is dropped).
void PmResetCounters(ULONGLONG now)
{
    PmMain& m = g_pm;
    for (auto& v : g_pmCount) v.store(0, std::memory_order_relaxed);
    for (auto& v : g_pmHist) v.store(0, std::memory_order_relaxed);
    m.calls = m.framesB = m.alphaPairs = m.alphaDiff = 0;
    for (auto& v : m.phases) v = 0;
    for (auto& v : m.why) v = 0;
    m.alphaMax = 0;
    m.logStart = now;
    m.logAt = now + 60000;
}

// The counters since the last line, then a new window.
void PmLogCounters(ULONGLONG now)
{
    PmMain& m = g_pm;
    uint64_t n[kPmCounts], hist[kPmBins], moved = 0;
    for (int i = 0; i < kPmCounts; ++i) n[i] = g_pmCount[i].exchange(0, std::memory_order_relaxed);
    for (int i = 0; i < kPmBins; ++i) {
        hist[i] = g_pmHist[i].exchange(0, std::memory_order_relaxed);
        moved += hist[i];
    }
    if (m.calls) {
        std::string why;
        for (int i = 0; i < kPmWhyN; ++i)
            if (m.why[i]) {
                char b[112];
                snprintf(b, sizeof(b), "%s%s %llu", why.empty() ? "" : ", ", kPmWhyName[i], (unsigned long long)m.why[i]);
                why += b;
            }
        Log("Engine: stereoParticleMotion=%d (particleMotionLog), %.0f s: %llu frames B - own particle motion given "
            "back in %llu, measured only in %llu; %llu frames A noted; not done: %s", m.logMode,
            (now - m.logStart) / 1000.0, (unsigned long long)m.framesB, (unsigned long long)m.phases[kPmRestore],
            (unsigned long long)m.phases[kPmMeasure], (unsigned long long)m.phases[kPmRecord],
            why.empty() ? "none" : why.c_str());
        float p50 = 0, p90 = 0;
        uint64_t cum = 0;
        for (int k = 0; k < kPmBins && moved; ++k) {
            cum += hist[k];
            if (p50 == 0 && cum * 2 >= moved) p50 = PmBinValue(k);
            if (cum * 10 >= moved * 9) {
                p90 = PmBinValue(k);
                break;
            }
        }
        int width = 0;
        const float f = PmFocalPx(width);
        char fText[48];
        if (width) snprintf(fText, sizeof(fText), "f %.0f px, %d px wide", f, width);
        else snprintf(fText, sizeof(fText), "f %.0f px assumed", f);
        Log("Engine: particle motion: frame A noted %llu particles (%llu new: never drawn); frame B drew %llu with a "
            "note, %llu of them (%.1f %%) had moved since the right eye's previous frame (more than 0.01 mm: median "
            "%.2f px, 90 %% under %.2f px; %s), %llu given that position back, %llu new (no motion, as before), %llu "
            "without a note (not drawn in frame A, or born since), %llu beyond the table; writes in frame A: %llu "
            "(must be 0); %llu tasks (%llu of another system); the game's alpha differs from frame A's (bits) in %llu "
            "of %llu frames B (max %.3g)",
            (unsigned long long)n[kPmNoted], (unsigned long long)n[kPmNotedNew], (unsigned long long)n[kPmChecked],
            (unsigned long long)n[kPmMoved], n[kPmChecked] ? 100.0 * n[kPmMoved] / n[kPmChecked] : 0.0, p50 * f,
            p90 * f, fText, (unsigned long long)n[kPmGiven], (unsigned long long)n[kPmKeptNew],
            (unsigned long long)n[kPmNoNote], (unsigned long long)n[kPmBeyond], (unsigned long long)n[kPmWritesA],
            (unsigned long long)n[kPmTasks], (unsigned long long)n[kPmOther], (unsigned long long)m.alphaDiff,
            (unsigned long long)m.alphaPairs, m.alphaMax);
    }
    PmResetCounters(now);
}

// The table, at the first use (main thread): VirtualAlloc's pages are zero (no pair noted) and committed when touched.
bool PmTable()
{
    if (g_pmTab) return true;
    if (g_pm.tableFailed) return false;
    g_pmTab = (PmEntry*)VirtualAlloc(nullptr, sizeof(PmEntry) * kPmSystems * kPmSlots, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
    if (!g_pmTab) {
        g_pm.tableFailed = true;
        Log("Engine: stereoParticleMotion: no memory for its table (%llu bytes) - the right eye's particles keep no "
            "motion of their own, as before", (unsigned long long)(sizeof(PmEntry) * kPmSystems * kPmSlots));
        return false;
    }
    return true;
}

// The owner's systems ({ int count; System** (+8) }, as the no-step loop reads them), under SEH: the tasks look their
// system up there (one not found - a fourth one, or some later layout - is left as it is).
bool PmReadSystems(BYTE* ps)
{
    __try {
        const int n = *(const int*)(ps + kPsCount);
        BYTE** list = *(BYTE***)(ps + kPsSystems);
        for (int i = 0; i < kPmSystems; ++i)
            g_pmSys[i].store(list && i < n ? list[i] : nullptr, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Hook_PsUpdate (main thread), before the update creates this frame's mesh tasks: what they do. Frame A of a stereo
// pair notes; its frame B gives back (stereoParticleMotion=1) or only measures (counters on, 0); else nothing.
void ParticleMotionDecide(BYTE* ps)
{
    if (!Real_MeshTask) return;
    PmMain& m = g_pm;
    const int mode = ParticleMotionMode();
    const bool log = ParticleMotionLogOn();
    const bool eyeB = g_rightThisFrame;
    if (mode != m.loggedMode) {
        Log("Engine: stereoParticleMotion=%d%s: %s", mode, FrameB::FileParticleMotion() >= 0 ? " (control file)" : "",
            mode ? "the right eye's particles get their own motion (frame B gives them back the positions the right "
                   "eye drew at the previous pair)"
                 : "as before: the right eye's particles have no motion of their own (the camera's only)");
        m.loggedMode = mode;
    }
    if (!m.brokenLogged && g_pmBroken.load(std::memory_order_relaxed)) {
        m.brokenLogged = true;
        Log("Engine: stereoParticleMotion: exception in a particle mesh task - off until the game restarts (the right "
            "eye's particles have no motion of their own again, as before)");
    }
    // The counters: a window opens when they are turned on; a line at its end (a minute) or when the mode changes
    // (each A/B segment of a test its own line).
    const ULONGLONG now = log ? GetTickCount64() : 0;
    if (log && !m.logOn) PmResetCounters(now);
    else if (log && (now >= m.logAt || mode != m.logMode)) PmLogCounters(now);   // (labelled with the mode it counted)
    m.logOn = log;
    m.logMode = mode;
    int phase = kPmNone, why = -1;
    if (mode > 0 || log) {
        if (g_pmBroken.load(std::memory_order_relaxed)) {
            why = kPmWhyBroken;
        } else if (!ps || !g_gameObj || !g_vrActive || !g_ready || !Cfg().syncParticles || !g_gameStep || !g_gameAlpha) {
            why = kPmWhySync;
        } else if (!PmReadSystems(ps)) {
            g_pmBroken.store(true, std::memory_order_relaxed);   // (logged at the next call)
            why = kPmWhyBroken;
        } else if (!PmTable()) {
            why = kPmWhyTable;
        } else {
            const int step = *(const int*)(g_gameObj + g_gameStep);
            uint32_t alpha;
            memcpy(&alpha, g_gameObj + g_gameAlpha, sizeof(alpha));
            if (!eyeB) {
                if (!g_leftThisFrame) {
                    why = kPmWhyNoPair;
                } else {
                    phase = kPmRecord;
                    m.pairA = (uint32_t)g_frameCounter;
                    m.stepA = step;
                    m.alphaA = alpha;
                }
            } else if (!m.pairA || m.pairA != (uint32_t)(g_frameCounter - 1)) {
                why = kPmWhyNoA;
            } else if (step != m.stepA) {
                why = kPmWhyStep;
            } else if (!Cfg().perEyeHistory || !g_historyOk || !g_histRightOwn) {
                // (g_histRightOwn: the previous frame B really had its own history - this frame's swap is still to
                // come, after its mesh tasks; a buffer that cannot be made any more is seen one pair late)
                why = kPmWhyHistory;
            } else {
                phase = mode > 0 ? kPmRestore : kPmMeasure;
                if (log) {
                    ++m.alphaPairs;
                    if (alpha != m.alphaA) {
                        float a, b;
                        memcpy(&a, &alpha, sizeof(a));
                        memcpy(&b, &m.alphaA, sizeof(b));
                        ++m.alphaDiff;
                        m.alphaMax = std::max(m.alphaMax, std::fabs(a - b));
                    }
                }
                if (mode > 0 && !m.firstGiven) {
                    m.firstGiven = true;
                    Log("Engine: stereoParticleMotion=1: first frame B whose particles get the right eye's own motion "
                        "(engine frame %llu)", (unsigned long long)g_frameCounter);
                }
            }
        }
    }
    g_pmState.store((uint64_t)g_frameCounter << 8 | (log ? kPmLog : 0) | (eyeB ? kPmEyeB : 0) | (uint64_t)phase,
                    std::memory_order_release);
    if (!log) return;
    ++m.calls;
    if (eyeB) ++m.framesB;
    if (phase != kPmNone) ++m.phases[phase];
    if (why >= 0) ++m.why[why];
}

} // namespace

// stereoParticleMotion: MeshGenerationTask::run (+434160), which the fix hooks (pattern below, unique in .text): it
// picks the CPU or the GPU mesh generator from the system's mesh mode (+1B40220h) and passes it the task's range
// (+48h / +4Ch); its first 10 bytes, two whole instructions, hold no RIP-relative operand and no branch lands in
// them. Then each generator's parts the fix relies on, checked at their offsets below (2.0.1): the GPU generator
// (+4341d0) and the CPU generator (+434830) each clamp the range to the sorted count (+1AC0178h), loop over the
// sorted list (+1AC0180h) into the particle records (0A8h and 104h bytes each), read a particle's last position
// (+0Ch) and overwrite it before the loop's end. The task's vtable (.rdata) must list run once, in slot 1. And the
// systems' owner as ParticleSystem::update's no-step loop reads it (count at +0, list at +8: InitParticleSort's site).
void InitParticleMotion()
{
    // (its update hook gives the systems and decides each frame; without the right eye's own TAA history there is
    // nothing to correct)
    if (!Real_PsUpdate || !g_historyOk) return;
    if (Cfg().stereoParticleMotion <= 0 && !Cfg().particleMotionLog && !Cfg().debugLua && !FrameB::HooksWanted())
        return;   // (off, and no control file can turn it on)
    BYTE* fn = ScanOnce("48 8B 41 40 8B 90 20 02 B4 01 85 D2 75 0F 44 8B 41 4C 8B 51 48 48 8B C8 E9 ?? ?? ?? ?? "
                        "83 FA 01 75 0F 44 8B 41 4C 8B 51 48 48 8B C8 E9 ?? ?? ?? ?? C3");
    BYTE* cpu = fn ? RelTarget(fn + 29, fn + 25) : nullptr;
    BYTE* gpu = fn ? RelTarget(fn + 49, fn + 45) : nullptr;
    auto inText = [](const BYTE* p, size_t n) { return p && p >= g_text && p + n <= g_text + g_textSize; };
    const bool gpuOk = inText(gpu, 0x600) &&
        Matches(gpu, "4C 8B DC 41 55 41 56 41 57 48 81 EC E0 00 00 00 44 8B B9 78 01 AC 01 4C 8B F1 45 3B C7 45 8B EF "
                     "45 0F 42 E8 41 3B D7 44 0F 42 FA 45 3B FD 0F 83") &&
        Matches(gpu + 0xa0, "49 8B 86 80 01 AC 01 F3 41 0F 10 B6 18 02 B4 01 49 63 CF 48 63 54 C8 04 48 69 DA A8 00 00 00 "
                            "48 69 FA 04 01 00 00 49 03 5E 08") &&
        Matches(gpu + 0x320, "F2 0F 10 43 0C F3 41 0F 5F DE F2 0F 11 44 2E 14 41 0F 28 E8 8B 43 14 89 44 2E 1C") &&
        Matches(gpu + 0x5ac, "F2 44 0F 11 63 0C 44 89 63 14 45 3B FD 0F 82") &&
        RelTarget(gpu + 0x5bf, gpu + 0x5bb) == gpu + 0xa0;
    const bool cpuOk = inText(cpu, 0x1580) &&
        Matches(cpu, "4C 8B DC 55 53 41 54 49 8D AB D8 FD FF FF 48 81 EC 10 03 00 00 44 8B A1 78 01 AC 01") &&
        Matches(cpu + 0x55, "48 8B D9 48 C7 45 70 00 00 00 00 44 0F 42 E2") &&
        Matches(cpu + 0x110, "48 8B 83 80 01 AC 01 0F 57 E4 0F 57 DB 49 63 CC 41 0F 28 F0 45 0F 28 D8 44 8B 6C C8 04 "
                             "49 63 C5 48 69 F8 A8 00 00 00 48 69 F0 04 01 00 00 48 03 7B 08") &&
        Matches(cpu + 0x4f4, "F3 0F 10 57 38 F3 0F 5C 67 10 F3 0F 5C 4F 14 F3 0F 5C 6F 0C F2 0F 11 5F 0C 89 4F 14") &&
        Matches(cpu + 0x1517, "44 3B 65 E8 0F 82") && RelTarget(cpu + 0x1521, cpu + 0x151d) == cpu + 0x110;
    // The task's vtable: run listed once in .rdata, in slot 1.
    BYTE* vtSlot = nullptr;
    int refs = 0;
    if (fn) {
        auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (strncmp((const char*)sec->Name, ".rdata", 6) != 0) continue;
            BYTE* r = g_base + sec->VirtualAddress;
            for (size_t k = 16; k + 8 <= sec->Misc.VirtualSize; k += 8) {
                if (*(BYTE* const*)(r + k) != fn) continue;
                ++refs;
                if (inText(*(BYTE* const*)(r + k - 8), 1) && !inText(*(BYTE* const*)(r + k - 16), 1)) vtSlot = r + k;
            }
        }
    }
    // The systems' owner: the no-step loop of ParticleSystem::update (cmp [rdi],ebx ; ... mov rcx,[rdi+8] ;
    // mov rcx,[r15+rcx]), as InitParticleSort finds it.
    BYTE* loop = ScanOnce("8B DE 39 1F 76 ?? 66 90 48 63 C3 4C 8D 3C C5 00 00 00 00 48 8B 4F 08 49 8B 0C 0F 4C 3B F5 75 ?? "
                          "E8 ?? ?? ?? ?? 48 8B 47 08 49 8B 0C 07 48 89 74 24 60 4C 8D 44 24 60 48 8D 57 40 E8");
    if (!fn || !gpuOk || !cpuOk || refs != 1 || !vtSlot || !loop) {
        Log("Engine: particle mesh tasks not recognised (task %p, GPU mesh %p %d, CPU mesh %p %d, vtable references %d "
            "slot %p, no-step loop %p) - the right eye's particles keep no motion of their own, as before", fn, gpu,
            gpuOk, cpu, cpuOk, refs, vtSlot, loop);
        return;
    }
    // (MinHook reports a hook another design already put there as a success without the original)
    if (!HookFunction(fn, (void*)Hook_MeshTask, (void**)&Real_MeshTask) || !Real_MeshTask) {
        Real_MeshTask = nullptr;
        Log("Engine: particle mesh tasks not hooked (MinHook failed, or another hook is there) - the right eye's "
            "particles keep no motion of their own, as before");
        return;
    }
    Log("Engine: right eye's own particle motion ready (mesh task +%llx, its vtable slot +%llx; GPU mesh +%llx, CPU mesh "
        "+%llx; stereoParticleMotion=%d, particleMotionLog %d)", (unsigned long long)(fn - g_base),
        (unsigned long long)(vtSlot - g_base), (unsigned long long)(gpu - g_base), (unsigned long long)(cpu - g_base),
        ParticleMotionMode(), ParticleMotionLogOn() ? 1 : 0);
}

// skipShapeJobsB: the jobs' start in Scene::preRender: mov rcx,r13 ; call build-and-start ; mov rbx,[r13+0A0h] ;
// cmp [rbx+10h],r12d ; je (the next group, scene+0A0h); that function's start (its group at scene+0A8h, the
// one Scene::waitShapeInterpolation waits for); and the job's work per shape (0xd5a8c), all of it, since it is
// done by hand: test byte [rbx+0C8h],60h ; je ; lea rdx,[rsp+20h] ; mov rcx,rbx ; call transform ;
// mov rax,[game] ; cmp byte [rax+screenshot],0 ; jne ; cmp byte [rbx+174h],0 ; je ; +158h = +13Ch (28 bytes) ;
// jmp ; +158h = new ; +13Ch = new ; mov byte [rbx+174h],1.
void InitShapeJobSkip()
{
    if (!g_historyOk || !g_waitShapes) return;   // (FixObjectMotion, Hook_Render and the wait)
    if (!FrameB::HooksWanted()) return;          // (a render-only frame B needs the frame-B module)
    BYTE* site = ScanOnce("49 8B CD E8 ?? ?? ?? ?? 49 8B 9D A0 00 00 00 44 39 63 10 0F 84");
    BYTE* fn = site ? RelTarget(site + 8, site + 4) : nullptr;
    const bool start = fn && Matches(fn, "48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 48 4C 8B F9 33 ED "
                                         "44 8B E5 89 AC 24 A0 00 00 00 48 8B 99 A8 00 00 00");
    BYTE* job = ScanOnce("F6 83 C8 00 00 00 60 0F 84 ?? ?? ?? ?? 48 8D 54 24 20 48 8B CB E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
                         "80 B8 ?? ?? 00 00 00 75 2F 80 BB 74 01 00 00 00 74 26 0F 10 83 3C 01 00 00 8B 83 54 01 00 00 "
                         "F2 0F 10 8B 4C 01 00 00 0F 11 83 58 01 00 00 F2 0F 11 8B 68 01 00 00 EB 1E 0F 10 44 24 20 "
                         "0F 11 83 58 01 00 00 F2 0F 10 4C 24 30 F2 0F 11 8B 68 01 00 00 8B 44 24 38 89 83 70 01 00 00 "
                         "0F 10 44 24 20 0F 11 83 3C 01 00 00 F2 0F 10 4C 24 30 F2 0F 11 8B 4C 01 00 00 8B 44 24 38 "
                         "89 83 54 01 00 00 C6 83 74 01 00 00 01");
    if (!start || !job || U32(job + 35) >= 0x10000) {
        Log("Engine: shape interpolation jobs not recognised (site %p, start %p, job %p) - frame B starts them, as before",
            site, fn, job);
        return;
    }
    g_shapeXf = (PFN_ShapeXf)RelTarget(job + 26, job + 22);
    g_gameGlobal = (BYTE**)RelTarget(job + 33, job + 29);
    g_offScreenshot = U32(job + 35);
    g_shapeJobsRet = site + 8;
    // (MinHook reports a hook another design already put there as a success without the original)
    if (!HookFunction(fn, (void*)Hook_ShapeJobs, (void**)&Real_ShapeJobs) || !Real_ShapeJobs) {
        Log("Engine: shape interpolation jobs not hooked (MinHook failed, or another hook is there) - frame B starts "
            "them, as before");
        return;
    }
    Log("Engine: frame B shape interpolation skip ready (jobs +%llx, transform +%llx, screenshot game+%x; "
        "skipShapeJobsB=%d)", (unsigned long long)(fn - g_base), (unsigned long long)((BYTE*)g_shapeXf - g_base),
        g_offScreenshot, ShapeJobsMode());
}

// skipDeadPasses. Renderer::render's bloom and post-processing calls (+897a4) and its depth of field call (+8957c),
// found by the two patterns below (the renderer's bloom and depth of field objects at +0A38h / +0A28h, the frame
// index at +2210h). Then each function's parts the proof relies on (the offsets above), and the backend's clear /
// copy / fence.
void InitDeadPasses()
{
    if (!g_historyOk) return;   // (Hook_Render tells the hooks their renderer)
    if (!Cfg().skipDeadPasses && !FrameB::HooksWanted()) return;   // (off, and no control file can turn it on)
    BYTE* bs = ScanOnce("41 8B 87 10 22 00 00 4D 8B 84 C7 98 0A 00 00 F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 44 24 20 41 0F 28 D8 "
                        "4D 8B 40 38 49 8B D6 49 8B 8F 38 0A 00 00 E8 ?? ?? ?? ?? 49 8B D6 49 8B CF E8");
    BYTE* stub = bs ? RelTarget(bs + 52, bs + 48) : nullptr;
    BYTE* bloomFn = stub && Matches(stub, "FF 81 B0 00 00 00 E9") ? RelTarget(stub + 11, stub + 7) : nullptr;
    BYTE* postFn = bs ? RelTarget(bs + 63, bs + 59) : nullptr;
    BYTE* ds = ScanOnce("49 8B 9F 28 0A 00 00 49 8D 8F 18 22 00 00 E8 ?? ?? ?? ?? 41 8B 87 10 22 00 00 49 8B 94 C7 50 0A 00 00 "
                        "F3 0F 11 44 24 20 4D 8B 8F 48 09 00 00 4C 8B 42 38 49 8B 97 78 0A 00 00 48 8B CB E8");
    BYTE* dofFn = ds ? RelTarget(ds + 66, ds + 62) : nullptr;
    // Bloom::render: its result FBO (+38h) bound, then its texture cleared by the backend's +60h (backend, texture, colour).
    const bool bloomOk = bloomFn &&
        Matches(bloomFn, "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 A0 48 81 EC 60 01 00 00") &&
        ScanIn(bloomFn, 0x500, "48 8B 0E 45 33 C9 48 8B 56 38 41 B8 FF FF FF FF 4C 89 6C 24 20 48 8B 01 FF 50 30 48 8B 0E "
                               "4C 8D 05 ?? ?? ?? ?? 48 8B 56 38 48 8B 01 48 8B 52 38 FF 50 60");
    // The post pass: mov rax,[rcx+2200h] (the scene) ; ... movss xmm10,[rax+0A4Ch] (bloom) ; mov rbx,[game] ;
    // cmp byte [rbx+8A0h],0 (screenshot) ; ... movss [rsp+58h],xmm10 (mubPostParams.z) ; ... mov rcx,[rsi+0A38h] ;
    // call (mov rax,[rcx+38h] ; ret: the bloom result).
    BYTE* shot = postFn ? ScanIn(postFn, 0x100, "F3 44 0F 10 90 4C 0A 00 00 48 8B 1D ?? ?? ?? ?? 80 BB A0 08 00 00 00") : nullptr;
    BYTE* res = postFn ? ScanIn(postFn, 0x300, "48 8B 8E 38 0A 00 00 E8") : nullptr;
    const bool postOk = postFn && shot && res &&
        Matches(postFn, "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 C8 48 81 EC 30 01 00 00") &&
        ScanIn(postFn, 0x50, "48 8B 81 00 22 00 00") && ScanIn(postFn, 0x200, "F3 44 0F 11 54 24 58") &&
        Matches(RelTarget(res + 12, res + 8), "48 8B 41 38 C3");
    // Dof::render: its parameters (+0, +4, +8: mubDofParams.xyz) and backend (+18h); its noise angle:
    // mov rax,[game] ; cmp [rax+1E8h],r12b ; je ; cmp [rax+8A0h],r12b ; je ; mov eax,[rax+1F0h] ; (% 64) ; movss [angle],xmm0.
    BYTE* ang = dofFn ? ScanIn(dofFn, 0x400, "48 8B 05 ?? ?? ?? ?? 44 38 A0 E8 01 00 00 74 09 44 38 A0 A0 08 00 00 74 23 "
                                            "8B 80 F0 01 00 00 25 3F 00 00 80 7D 07 FF C8 83 C8 C0 FF C0 66 0F 6E C0 0F 5B C0 "
                                            "F3 0F 11 05") : nullptr;
    const bool dofOk = dofFn && ang &&
        Matches(dofFn, "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B F1") &&
        ScanIn(dofFn, 0x80, "F3 0F 10 06 4C 8D 44 24 40 48 8B 4E 18 41 B9 10 00 00 00 F3 0F 10 4E 04 F3 0F 10 56 08");
    // TRendererD3D12 +60h, clearTexture(backend, texture, colour): its barrier tracker (+1130h), a colour texture test (+1Bh).
    BYTE* clear = ScanOnce("48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83 EC 50 48 8B F9 C6 44 24 7C 00 4C 8B 4C 24 78 "
                           "48 81 C1 30 11 00 00 F6 42 1B 04 4D 8B F0 48 8B DA");
    // endRender: queue (+E50h) ->Signal(fence +E58h, value +E68h), then "in a frame" (+CCh) = 0.
    BYTE* sig = ScanOnce("48 8B 8B 50 0E 00 00 4C 8B 83 68 0E 00 00 48 8B 93 58 0E 00 00 48 8B 01 FF 50 70");
    const bool frameOk = sig && ScanIn(sig, 0x200, "C6 83 CC 00 00 00 00");
    // The debug view (+900b0): mov r15d,[rsi+9D8h] ; lea rbx,[rsi+978h] ; mov rcx,rbx ; lea rdx,["rawlight"].
    BYTE* dbg = ScanOnce("44 8B BE D8 09 00 00 48 8D 9E 78 09 00 00 48 8B CB 48 8D 15");
    BYTE** game1 = shot ? (BYTE**)RelTarget(shot + 16, shot + 12) : nullptr;
    BYTE** game2 = ang ? (BYTE**)RelTarget(ang + 7, ang + 3) : nullptr;
    if (!bloomOk || !postOk || !dofOk || !clear || !frameOk || !dbg || !game1 || game1 != game2) {
        Log("Engine: dead passes not recognised (bloom %d, post %d, depth of field %d, clear %p, frame %d, debug view %p, "
            "game %p/%p) - the bloom chain and the depth of field are drawn as before", bloomOk, postOk, dofOk, clear,
            frameOk, dbg, game1, game2);
        return;
    }
    g_dpGame = game1;
    g_dpDofAngle = (float*)RelTarget(ang + 60, ang + 56);
    g_dpClear = (PFN_ClearTexture)clear;
    // TRendererD3D12 +140h, copy(backend, dst FBO, src FBO, index, 0): rbp = dst texture, rdi = src texture, both through
    // the tracker (+1130h: COPY_DEST / COPY_SOURCE), CopyTextureRegion on the frame's list (+E40h). (skipDeadPasses=2 only)
    BYTE* copy = ScanOnce("4C 8B DC 49 89 5B 10 49 89 6B 18 56 57 41 56 48 81 EC C0 00 00 00 48 8B 6A 38 45 33 F6 41 8B C1 "
                          "48 8B F1 45 89 73 B8 48 8B D5 44 88 B4 24 E4 00 00 00 48 81 C1 30 11 00 00 4C 8B 8C 24 E0 00 00 00 "
                          "49 8B 7C C0 38 48 8B 45 38");
    const bool copyOk = copy && ScanIn(copy, 0x120, "48 8B 8E 40 0E 00 00 48 8D 54 24 40") &&
                        ScanIn(copy, 0x120, "FF 90 80 00 00 00");
    g_dpCopy = copyOk ? (PFN_CopyFbo)copy : nullptr;
    g_dpChk.ready = copyOk && g_releaseFbo != nullptr;
    // (MinHook reports a hook another design already put there as a success without the original)
    if (!HookFunction(bloomFn, (void*)Hook_BloomRender, (void**)&Real_BloomRender) || !Real_BloomRender ||
        !HookFunction(dofFn, (void*)Hook_DofRender, (void**)&Real_DofRender) || !Real_DofRender ||
        !HookFunction(postFn, (void*)Hook_PostPass, (void**)&Real_PostPass) || !Real_PostPass) {
        Log("Engine: dead passes not hooked (MinHook failed, or another hook is there) - the bloom chain and the depth "
            "of field are drawn as before");
        return;
    }
    g_dpOn = true;
    Log("Engine: dead passes ready (bloom +%llx, depth of field +%llx, post +%llx, clear +%llx; check %s; "
        "skipDeadPasses=%d)", (unsigned long long)(bloomFn - g_base), (unsigned long long)(dofFn - g_base),
        (unsigned long long)(postFn - g_base), (unsigned long long)(clear - g_base),
        g_dpChk.ready ? "ready" : "NOT available", DeadPassMode());
}

// stereoSameNoise. Renderer::render's TemporalAa::update call (+37a, inside the fields InitHistoryHook checked at
// +373) and its scene constants call (+75e), then each function's parts the fix relies on.
void InitSameNoise()
{
    if (!g_historyOk || !g_renderFn) return;
    BYTE* render = g_renderFn;
    // render+373h: mov rcx,[r15+0A30h] ; call TemporalAa::update (checked by InitHistoryHook)
    BYTE* upd = RelTarget(render + 0x37f, render + 0x37b);
    // TemporalAa::update(taa, ...): its prologue, then the jitter counter (the TAA's +0) incremented and taken % 8
    // after a test of a global Halton flag, and its stores to +18h / +1Ch (patterns below).
    const bool updOk = Matches(upd, "48 8B C4 53 48 81 EC 90 00 00 00 0F 29 78 E8 48 8B D9 8B 09") &&
                       ScanIn(upd, 0x200, "FF C1 80 3D ?? ?? ?? ?? 00 0F 84") &&
                       ScanIn(upd, 0x200, "81 E1 07 00 00 80 7D 07 FF C9 83 C9 F8 FF C1 89 0B") &&
                       ScanIn(upd, 0x200, "F3 0F 11 5B 18 F3 0F 59 7B 1C F3 0F 11 7B 1C");
    // render+75bh: mov rcx,r15 ; call +92b80 ; mov rcx,r15 ; call (the scene constants) ; mov eax,[r15+0A18h]
    BYTE* site = ScanIn(render + 0x740, 0x40, "49 8B CF E8 ?? ?? ?? ?? 49 8B CF E8 ?? ?? ?? ?? 41 8B 87 18 0A 00 00");
    BYTE* cb = site ? RelTarget(site + 16, site + 12) : nullptr;
    // its +1ah: mov rax,[game] ; mov rbx,rcx ; cmp byte [rax+1E8h],0 (paused) ; je ; cmp byte [rax+8A0h],0 (photo) ;
    // je ; mov rax,[rcx+0A30h] (the TAA) ; mov edx,[rax] (its counter) ; mov [noise frame],edx
    const bool cbOk = cb && Matches(cb + 0x1a, "48 8B 05 ?? ?? ?? ?? 48 8B D9 80 B8 E8 01 00 00 00 74 09 80 B8 A0 08 00 00 00 "
                                              "74 0F 48 8B 81 30 0A 00 00 8B 10 89 15");
    const int* rnd = cbOk ? (const int*)RelTarget(cb + 0x45, cb + 0x41) : nullptr;
    // ... movd xmm0,[noise frame] ; cvtdq2ps xmm0,xmm0 ; movss [rbp+0DCh],xmm0 (mubFrameParams.w: +1ACh of the 1F4h-byte
    // SceneDynamicBuffer, uploaded with mov r9d,1F4h)
    BYTE* w = rnd ? ScanIn(cb, 0x400, "66 0F 6E 05 ?? ?? ?? ?? 0F 5B C0 F3 0F 11 85 DC 00 00 00") : nullptr;
    const bool wOk = w && (const int*)RelTarget(w + 8, w + 4) == rnd && ScanIn(cb, 0x400, "41 B9 F4 01 00 00");
    if (!updOk || !cbOk || !wOk) {
        Log("Engine: noise frame not recognised (TAA update %d, scene constants %p, noise frame %p / %d) - the right eye "
            "keeps its own noise frame (stereoSameNoise has no effect)", updOk, cb, rnd, wOk);
        return;
    }
    g_sameNoise.frameRnd = rnd;
    g_sameNoise.ok = true;
    Log("Engine: same noise in both eyes ready (TAA update +%llx, scene constants +%llx, noise frame +%llx; "
        "stereoSameNoise=%d)", (unsigned long long)(upd - g_base), (unsigned long long)(cb - g_base),
        (unsigned long long)((const BYTE*)rnd - g_base), SameNoiseMode());
}

void InitHistoryHook()
{
    // Game draw: mov r9,r12 ; mov r8,[rsi+50h] ; mov rdx,[rsi+888h] ; mov rcx,[r13+0] ; call Renderer::render
    BYTE* site = Scan("4D 8B CC 4C 8B 46 50 48 8B 96 88 08 00 00 49 8B 4D 00 E8 ?? ?? ?? ??");
    BYTE* render = site ? RelTarget(site + 23, site + 19) : nullptr;
    // Sanity checks of every field the swap relies on (Teardown 2.0.1 layout).
    bool ok = render &&
        Matches(render + 0x50, "48 FF 81 08 22 00 00 8B 89 14 22 00 00 41 8B 87 10 22 00 00 41 89 87 14 22 00 00 41 89 8F 10 22 00 00") &&
        Matches(render + 0x299, "41 0F 10 87 EC 22 00 00 41 0F 11 87 2C 23 00 00") &&
        Matches(render + 0x373, "49 8B 8F 30 0A 00 00 E8 ?? ?? ?? ?? 49 8D 9F 58 22 00 00 0F 10 03 41 0F 11 87 6C 23 00 00") &&
        ScanIn(render, 0x100, "45 38 B7 F1 23 00 00") != nullptr;
    // TemporalAa::createResources(taa, w, h)
    BYTE* create = Scan("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D 6C 24 B9 48 81 EC C0 00 00 00 "
                        "48 8B F1 89 51 10 44 89 41 14");
    // texture release: first "test rcx,rcx ; je ; call release" inside createResources
    BYTE* rel = create ? ScanIn(create, 0x200, "48 85 C9 74 ?? E8 ?? ?? ?? ??") : nullptr;
    // FBO release: just before a backend createFbo call: test rcx,rcx ; je ; call release ... mov rcx,[rbx+9C0h] ; mov rax,[rcx] ; lea r8,[name]
    BYTE* fboSite = Scan("48 85 C9 74 ?? E8 ?? ?? ?? ?? 90 66 C7 45 ?? 00 00 C7 45 ?? 01 00 00 00 C7 45 ?? ?? 00 00 00 "
                         "48 8B 8B C0 09 00 00 48 8B 01 4C 8D 05");
    ok = ok && create && rel && fboSite;
    if (!ok) {
        Log("Engine: per-eye history not available (render %p create %p release %p fbo %p)", render, create, rel, fboSite);
        return;
    }
    // render+0x4a0: mov rcx,[r15+2200h] ; call Scene::waitShapeInterpolation ; mov rdi,r12
    BYTE* waitSite = ScanIn(render, 0x600, "49 8B 8F 00 22 00 00 E8 ?? ?? ?? ?? 49 8B FC");
    BYTE* waitFn = waitSite ? RelTarget(waitSite + 12, waitSite + 8) : nullptr;
    // push rbx ; sub rsp,20h ; mov rbx,[rcx+0A8h] ; cmp dword ptr [rbx+10h],0 ; je
    if (waitFn && Matches(waitFn, "40 53 48 83 EC 20 48 8B 99 A8 00 00 00 83 7B 10 00 74"))
        g_waitShapes = (PFN_WaitShapes)waitFn;
    else
        Log("Engine: shape interpolation wait not recognised (site %p fn %p) - right-eye object fix may read unfinished transforms", waitSite, waitFn);
    g_taaCreate = (PFN_TaaCreate)create;
    g_releaseTex = (PFN_Release)RelTarget(rel + 10, rel + 6);
    g_releaseFbo = (PFN_Release)RelTarget(fboSite + 10, fboSite + 6);
    g_renderFn = render;
    if (!HookFunction(render, (void*)Hook_Render, (void**)&Real_Render)) return;
    g_historyOk = true;
    Log("Engine: per-eye history ready (render +%llx, TAA create +%llx)", (unsigned long long)(render - g_base),
        (unsigned long long)(create - g_base));
}

// The player's look yaw: read by the Lua binding of GetPlayerYaw
//   ... call GetPlayer ; test rax,rax ; je ; movss xmm1,[rax+704h] ; mov rcx,rdi ; mulss xmm1,[rad->deg]
void InitPlayerYaw()
{
    // Find the "GetPlayerYaw" name, then the registration thunk that references it
    // (lea r8,[impl] ; lea rdx,[name]), then the field read in the implementation.
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE* name = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !name; ++i, ++sec) {
        if (strncmp((const char*)sec->Name, ".rdata", 6) != 0) continue;
        BYTE* s = g_base + sec->VirtualAddress;
        for (size_t k = 0; k + 13 < sec->Misc.VirtualSize; ++k)
            if (memcmp(s + k, "GetPlayerYaw", 13) == 0 && s[k - 1] == 0) { name = s + k; break; }
    }
    if (!name) { Log("Engine: GetPlayerYaw not found - aim turns the player the old way"); return; }
    for (size_t i = 7; i + 7 < g_textSize; ++i) {
        BYTE* p = g_text + i;
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15 && RelTarget(p + 7, p + 3) == name && p[-7] == 0x4C &&
            p[-6] == 0x8D && p[-5] == 0x05) {
            BYTE* impl = RelTarget(p, p - 4);
            // The binding first resolves the player entity: call GetPlayerById(ctx, id). Id 0 (the local
            // player) is  mov rax,[g_game] ; movsxd rcx,[rax+168h] ; mov rax,[rax+0C0h] ; mov rax,[rax+rcx*8]
            BYTE* getPlayer = nullptr;
            for (BYTE* q = impl; q < impl + 0x40 && !getPlayer; ++q)
                if (q[0] == 0xE8 && q[5] == 0x48 && q[6] == 0x85 && q[7] == 0xC0) getPlayer = RelTarget(q + 5, q + 1);
            if (getPlayer)
                for (BYTE* q = getPlayer; q < getPlayer + 0x40; ++q)
                    if (q[0] == 0x48 && q[1] == 0x63 && q[2] == 0x88 && q[7] == 0x48 && q[8] == 0x8B && q[9] == 0x80 &&
                        q[14] == 0x48 && q[15] == 0x8B && q[16] == 0x04 && q[17] == 0xC8) {
                        g_offPlayerIndex = U32(q + 3);
                        g_offPlayerList = U32(q + 10);
                        break;
                    }
            for (BYTE* q = impl; q < impl + 0x60; ++q)
                if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x10 && q[3] == 0x88 && q[8] == 0x48 && q[9] == 0x8B) {
                    if (!g_offPlayerList) break;
                    g_offPlayerYaw = U32(q + 4);
                    Log("Engine: player yaw field %x, local player = game[%x][game[%x]] (GetPlayerYaw +%llx)", g_offPlayerYaw,
                        g_offPlayerList, g_offPlayerIndex, (unsigned long long)(impl - g_base));
                    return;
                }
        }
    }
    Log("Engine: player yaw field not recognised - aim turns the player the old way");
}

// ---------------------------------------------------------------------------
// Mods aim with the VR controller
// ---------------------------------------------------------------------------
// Mods aim with GetCameraTransform(): weapons, target designators (the AC130
// airstrike on the ground raycasts along the camera's forward axis to place the
// strike), creative mode's tools... On a desktop that is the player's camera,
// i.e. where the player aims. In VR the game's camera is the HEAD (main.lua moves
// it to the eye with SetCameraOffsetTransform) while the aim is the controller's
// laser: the strike went where the player looked, and moved with the head even
// with the laser held still. The Lua mod turns the player (yaw / pitch) so that
// the player's camera looks from its position straight at the laser's target
// (main.lua, updateAim): that camera IS the VR aim. So while the VR player is on
// foot and neither a mod (AC130 plane view, drones...) nor the map drives the
// camera, GetCameraTransform() returns the player's camera, as the game documents
// it for first person (script_defs.lua: "The player eye transform is the same as
// what you get from GetCameraTransform when playing in first-person"). The table
// is built by the engine's own GetPlayerCameraTransform for the calling script:
// nothing is written to the game, and both frames of a stereo pair get the
// player's aim instead of one eye each (the head camera alternates between the
// eyes). The aim the Lua mod sets in the left-eye frame reaches the player's
// camera at the next player update: while the hand moves, the two frames of a
// pair can differ by one controller update, as for the game's own guns. The
// rendered view stays the head. GetCameraTransform takes no argument: a call
// WITH one (the Lua mod's own calls) still gets the real camera.
//   GetCameraTransform (2.0.x +475f80): mov rax,[g_game] ; mov rbx,[rax+60h] ... mov r9d,3 ;
//     lea r8,[rbx+1D0h] (pos) ... mov r9d,4 ; lea r8,[rbx+1DCh] (rot)
//   GetPlayerCameraTransform (+478c10): call ArgInt(args, 0, 0) (player id) ; then the player's
//     camera (local player in first person: player+640h; v2 scripts: +678h, same rotation)
//   ArgInt (+631b80): cmp edx,[rcx+8] ; jge default ; mov rcx,[rcx] ; inc edx ; jmp lua_tointeger
//     (args+8 = number of Lua arguments)
namespace {

PFN_LuaFn Real_GetCameraTransform = nullptr;
PFN_LuaFn g_luaPlayerCamera = nullptr;  // GetPlayerCameraTransform's implementation (called, not hooked)
uint32_t g_offLuaArgCount = 0;          // binding arguments -> number of Lua arguments (8)
bool g_handAim = false;                 // vr.cpp: on foot, the Lua mod aims the player with the controller
uint64_t g_handAimFrame = 0;            // engine frame of the last EngineSetHandAim
uint32_t g_aimCamCalls[2] = {};         // diagnostics (debugLua): calls given the real / the aim camera

// VR on foot, aiming with the controllers: no mod camera, no map (the aim camera and dual wield).
bool VrAimOnFoot()
{
    // (EngineSetHandAim comes with every Lua request: stale once the Lua mod stops)
    if (!g_vrActive || !g_handAim || g_frameCounter - g_handAimFrame > 2 || !g_gameObj) return false;
    BYTE* camObj = *(BYTE**)(g_gameObj + g_offGamePlayer);
    if (!camObj) return false;
    int mode = *(int*)camObj;
    if (mode == g_scriptCamMode || (g_scriptMode && mode == g_scriptMode)) return false;   // a mod's own camera
    float blend;
    if (MapPhase(camObj, blend) != 0) return false;   // level map
    return LocalPlayer() != nullptr;
}

bool AimCameraNow() { return Cfg().modAimHand && VrAimOnFoot(); }

__int64 __fastcall Hook_GetCameraTransform(void* ctx, void* args, void* ret, void* a4)
{
    bool aim = args && *(const int*)((const BYTE*)args + g_offLuaArgCount) == 0 && AimCameraNow();
    static bool logged = false;
    if (aim && !logged) {
        logged = true;
        Log("Engine: a script reads the camera - it gets the VR aim (controller), the view stays the head");
    }
    if (Cfg().debugLua) {
        static uint64_t since = 0;
        ++g_aimCamCalls[aim ? 1 : 0];
        if (g_frameCounter - since >= 600) {
            Log("Engine: GetCameraTransform over %llu frames: %u calls got the VR aim, %u the real camera",
                (unsigned long long)(g_frameCounter - since), g_aimCamCalls[1], g_aimCamCalls[0]);
            since = g_frameCounter;
            g_aimCamCalls[0] = g_aimCamCalls[1] = 0;
        }
    }
    return aim ? g_luaPlayerCamera(ctx, args, ret, a4) : Real_GetCameraTransform(ctx, args, ret, a4);
}

void InitAimCamera()
{
    // (FindLuaBinding: the implementation passed with the name, "lea r8,[impl] ; lea rdx,[name]")
    BYTE* cam = FindLuaBinding("GetCameraTransform");
    BYTE* pcam = FindLuaBinding("GetPlayerCameraTransform");
    // GetCameraTransform returns the camera object's transform (the head, in VR):
    //   mov rax,[g_game] ; mov rbx,[rax+cam] ... mov r9d,3 ; lea r8,[rbx+pos] ... mov r9d,4 ; lea r8,[rbx+rot]
    BYTE* a = cam ? ScanIn(cam, 0x40, "48 8B 05 ?? ?? ?? ?? 48 8B 58 ??") : nullptr;
    BYTE* b = cam ? ScanIn(cam, 0x60, "41 B9 03 00 00 00 4C 8D 83 ?? ?? 00 00") : nullptr;
    BYTE* c = cam ? ScanIn(cam, 0xA0, "41 B9 04 00 00 00 4C 8D 83 ?? ?? 00 00") : nullptr;
    // GetPlayerCameraTransform first reads its player id: xor r8d,r8d ; xor edx,edx ; mov rcx,rax ; call ArgInt
    BYTE* d = pcam ? ScanIn(pcam, 0x30, "45 33 C0 33 D2 48 8B C8 E8") : nullptr;
    BYTE* argInt = d ? RelTarget(d + 13, d + 9) : nullptr;
    // (mod cameras and the local player must be recognised: never the player's camera during a mod's view)
    bool ok = a && b && c && argInt && g_offGamePlayer && a[10] == g_offGamePlayer && U32(b + 9) == g_offCamPos &&
              U32(c + 9) == g_offCamRot && Matches(argInt, "3B 51 ?? 7D ?? 48 8B 09 FF C2 E9") && g_scriptCamMode &&
              g_offPlayerList;
    if (!ok) {
        Log("Engine: GetCameraTransform not recognised (%p %p %p %p %p) - mods aim where the head looks", cam, pcam, a,
            b, d);
        return;
    }
    g_offLuaArgCount = argInt[2];
    g_luaPlayerCamera = (PFN_LuaFn)pcam;
    if (!HookFunction(cam, (void*)Hook_GetCameraTransform, (void**)&Real_GetCameraTransform)) {
        Real_GetCameraTransform = nullptr;
        return;
    }
    Log("Engine: mods aim with the VR controller (GetCameraTransform +%llx -> player camera +%llx, argument count %x)",
        (unsigned long long)(cam - g_base), (unsigned long long)(pcam - g_base), g_offLuaArgCount);
}

// --- Dual wield ------------------------------------------------------------------------------------
// GetPlayerEyeTransform (+4791b0: mov [rsp+8],rbx ; push rdi ; sub rsp,60h): ArgInt(args, 0, 0) (player
// id), the player (+4a4360), its eye at player+640h (v2 scripts: +678h), returned as {pos, rot}.
// GetPlayerCrouch (+4790f0): the same player, then movss xmm1,[rax+768h].
// A dual-wield tool (DualProfile: Total Overdose) fires from its eye. input.cpp turns each trigger
// press into one left click, one at a time (the gun of that hand: g_dualClickGun). Once that click
// reached the game (EngineDualClickDelivered), the first eyes the tool's script asks for in the next
// normal (not frozen) update - the one that sees the press: the mod fires on InputPressed - are its shot:
// they are answered with "eyes" that put the mod's muzzle on the drawn muzzle M of the gun of the hand
// that pressed, its direction d toward that hand's laser target:
//   eye.rot = R (-Z = d, +Y as close as possible to the gun's own up),
//   eye.pos = M - R x (side_k, 0, fwd) - (0, up + crouchUp x crouch, 0),   side_1 = +side, side_2 = -side,
// so whichever of its two cannons the mod's hidden alternation picks, the bullet leaves M toward the
// target (crouch: read like GetPlayerCrouch, this frame's value). M and the target come from the Lua
// mod (TDVRG, TDVRK) in the frame of the gun's own body - the tool, or the off hand's copy - so the
// dominant gun's recoil moves neither the off gun nor the targets. The table is built by the original
// GetCameraTransform (its trampoline) from the camera fields, written for the call and restored (as
// ShapeAsMade does): scripts run on the main thread, before the frame's render jobs read the camera.
PFN_LuaFn Real_GetPlayerEye = nullptr;
uint32_t g_offPlayerCrouch = 0;   // player entity -> crouch 0..1 (0x768; 0 = not recognised: crouch counted 0)

const char* DualHandName(int gun) { return gun == 1 ? "dominant" : "off"; }

// The body of gun `gun`: 1 = the tool in hand, 0 = the off hand's copy (TDVRV).
BYTE* DualBody(int gun)
{
    BYTE* b = nullptr;
    if (gun == 1) {
        BYTE* pl = LocalPlayer();
        b = pl ? *(BYTE**)(pl + kPlayerToolBody) : nullptr;
    } else if (g_gameObj && g_dual.copy) {
        b = EntityOfHandle(*(BYTE**)(g_gameObj + g_offGameScene), g_dual.copy);
    }
    return b && *(const uint16_t*)(b + kEntityType) == 1 ? b : nullptr;
}

Pose DualBodyPose(BYTE* b)
{
    const float* f = (const float*)(b + 0x40);   // body: world position, then rotation (GetBodyTransform)
    return { Normalize({ f[3], f[4], f[5], f[6] }), { f[0], f[1], f[2] } };
}

// Muzzle and target of that gun sent in the last 10 frames (5 stereo pairs).
bool DualFresh(int gun)
{
    const DualGun& g = g_dual.gun[gun & 1];
    return g.mFrame && g.tFrame && g_frameCounter - g.mFrame <= 10 && g_frameCounter - g.tFrame <= 10 &&
           (gun == 1 || g_dual.copy != 0);
}

bool DualAllowedNow()
{
    const DualState& D = g_dual;
    const DualProfile* p = D.prof;
    if (!D.hooked || !Cfg().dualWield || Cfg().aimMode != 0 || !p || g_frameCounter - D.profFrame > 2) return false;
    const int i = (int)(p - kDualProfiles);
    return i >= 0 && i < kDualProfileCount && !D.off[i] && VrAimOnFoot();
}

// Rotation whose -Z is f (unit), +Y as close as possible to up.
Quat LookRotation(Vec3 f, Vec3 up)
{
    const Vec3 z = f * -1.0f;
    Vec3 x = Cross(up, z);
    float l = Length(x);
    if (l < 1e-3f) { x = Cross(Vec3{ 0, 1, 0 }, z); l = Length(x); }
    if (l < 1e-3f) { x = Cross(Vec3{ 1, 0, 0 }, z); l = Length(x); }
    x = x * (1.0f / l);
    const Vec3 y = Cross(z, x);
    // (matrix of columns x, y, z)
    Quat q;
    const float tr = x.x + y.y + z.z;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = { (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s };
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        q = { 0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s };
    } else if (y.y > z.z) {
        const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        q = { (y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s };
    } else {
        const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
        q = { (z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s };
    }
    return Normalize(q);
}

// Muzzle (world) and eye rotation of a shot of gun `gun`; why = the reason when it cannot be given.
bool DualAim(int gun, Vec3& m, Quat& r, float& dist, const char*& why)
{
    if (!DualFresh(gun)) { why = "its muzzle or target not sent lately"; return false; }
    BYTE* b = DualBody(gun);
    if (!b) { why = gun ? "no tool body" : "its copy not found"; return false; }
    const Pose bp = DualBodyPose(b);
    const DualGun& g = g_dual.gun[gun];
    m = Mul(bp, Pose{ {}, g.m }).p;
    const Vec3 v = g.target - m;
    dist = Length(v);
    if (!(dist > 0.05f) || !(dist < 1e4f)) { why = "target on the muzzle"; return false; }
    r = LookRotation(v * (1.0f / dist), Rotate(bp.q, { 0, 1, 0 }));
    return true;
}

// The eye answered to the tool's script, or false (the real one: not a shot of ours).
bool DualEye(void* ctx, Pose& out)
{
    DualState& D = g_dual;
    const int gun = g_dualClickGun.load(std::memory_order_acquire);
    if (gun < 0 || g_inFrozenUpdate || !g_toolScriptCtx || ctx != g_toolScriptCtx || g_frameCounter - g_toolScriptFrame > 2)
        return false;
    // (our click delivered, and this is the first normal update since: the game sees the press there and
    // the mod fires on InputPressed - a later eye request of the mod is something else: its dash...)
    if (D.downId != g_dualClickId.load(std::memory_order_acquire)) return false;
    if (g_normalUpdates != D.downNormal + 1) {
        if (D.usedId != D.downId && g_normalUpdates > D.downNormal + 1 && g_normalUpdates <= D.downNormal + 3) {
            static int logs = 0;
            if (logs < 20) {
                ++logs;
                Log("Engine: dual: late eye - the tool's script asked for an eye %llu normal update(s) after click %u "
                    "reached the game, not in the first one: not taken for its shot",
                    (unsigned long long)(g_normalUpdates - D.downNormal), D.downId);
            }
        }
        return false;
    }
    if (!DualAllowedNow()) return false;
    if (D.usedId != D.downId) {
        // The first eye the tool's script asks for since our click reached the game: its shot.
        D.usedId = D.downId;
        D.shotFrame = g_frameCounter;
        D.shotGun = gun;
        D.calls = 0;
        ++D.shots;
        float dist = 0;
        const char* why = "";
        D.shotOk = DualAim(gun, D.shotM, D.shotR, dist, why);
        D.shotDist = dist;
        D.shotDataNow = D.gun[gun].tFrame == g_frameCounter;
        if (!D.shotOk) {
            static int logs = 0;
            if (logs < 30) {
                ++logs;
                Log("Engine: dual: shot #%d of the %s hand's gun NOT moved (%s) - it leaves from the head, as on a "
                    "desktop", D.shots, DualHandName(gun), why);
            }
        }
    } else if (D.shotFrame != g_frameCounter) {
        return false;   // (a later frame of the same click: not its shot)
    }
    const int k = D.calls++;   // 0 = the mod's first cannon (+ side), 1 = its second (- side)
    if (!D.shotOk || k >= D.prof->calls) {
        ++D.extraCalls;
        return false;
    }
    const float side = k == 0 ? D.prof->side : -D.prof->side;
    BYTE* pl = LocalPlayer();
    const float crouch = pl && g_offPlayerCrouch ? std::clamp(*(const float*)(pl + g_offPlayerCrouch), 0.0f, 1.0f) : 0.0f;
    out.q = D.shotR;
    out.p = D.shotM - Rotate(D.shotR, { side, 0, D.prof->fwd }) - Vec3{ 0, D.prof->up + D.prof->crouchUp * crouch, 0 };
    D.shotCrouch = crouch;
    ++D.overrides;
    return true;
}

__int64 __fastcall Hook_GetPlayerEye(void* ctx, void* args, void* ret, void* a4)
{
    Pose e;
    BYTE* camObj = g_gameObj ? *(BYTE**)(g_gameObj + g_offGamePlayer) : nullptr;
    if (!args || !camObj || *(const int*)((const BYTE*)args + g_offLuaArgCount) != 0 || !DualEye(ctx, e))
        return Real_GetPlayerEye(ctx, args, ret, a4);
    float* c = (float*)(camObj + g_offCamPos);   // position, then rotation (x y z w) at +12
    float saved[7];
    memcpy(saved, c, sizeof(saved));
    const float v[7] = { e.p.x, e.p.y, e.p.z, e.q.x, e.q.y, e.q.z, e.q.w };
    memcpy(c, v, sizeof(v));
    const __int64 r = Real_GetCameraTransform(ctx, args, ret, a4);
    memcpy(c, saved, sizeof(saved));
    return r;
}

// After the shot frame (Hook_Update, DualFrameEnd): what the shot did. The first one, and every one
// with debugLua (300 at most).
void DualShotLog()
{
    const DualState& D = g_dual;
    static int logs = 0;
    if (!D.shotOk || (logs > 0 && !Cfg().debugLua) || logs >= 300) return;
    ++logs;
    const Vec3 m = D.shotM, d = Rotate(D.shotR, { 0, 0, -1 });
    Log("Engine: dual: shot #%d by the %s hand's gun (click %u, %llu frame(s) after it reached the game): %d eye "
        "call(s), muzzle %.3f %.3f %.3f, direction %.3f %.3f %.3f, target %.2f m away, crouch %.2f; aim data %s; "
        "off-hand shots' kicks held back so far %u", D.shots, DualHandName(D.shotGun), D.usedId,
        (unsigned long long)(D.shotFrame - D.downFrame), D.calls, m.x, m.y, m.z, d.x, d.y, d.z, D.shotDist,
        D.shotCrouch, D.shotDataNow ? "of this frame (the Lua mod ran before the tool's script)"
                                    : "of an earlier frame (the tool's script ran before the Lua mod)", D.kicksHeld);
}

void InitDualWield()
{
    BYTE* eye = FindLuaBinding("GetPlayerEyeTransform");
    BYTE* crouch = FindLuaBinding("GetPlayerCrouch");
    // The eye: "mov [rsp+8],rbx ; push rdi ; sub rsp,X" then its player id: xor r8d,r8d ; xor edx,edx ; mov rcx,rax ; call ArgInt
    BYTE* a = eye ? ScanIn(eye, 0x30, "45 33 C0 33 D2 48 8B C8 E8") : nullptr;
    // The crouch: call player ; test rax,rax ; je ; movss xmm1,[rax+X]
    BYTE* c = crouch ? ScanIn(crouch, 0x40, "48 85 C0 74 ?? F3 0F 10 88 ?? ?? 00 00") : nullptr;
    const bool ok = eye && a && Matches(eye, "48 89 5C 24 08 57 48 83 EC") && Real_GetCameraTransform && g_offCamPos &&
                    g_offLuaArgCount && g_entPosArray && g_offPlayerList;
    if (!ok) {
        Log("Engine: dual: GetPlayerEyeTransform not recognised (%p %p; camera %d, handles %d) - Total Overdose keeps "
            "both guns in the dominant hand, fired as before", eye, a, Real_GetCameraTransform ? 1 : 0, g_entPosArray ? 1 : 0);
        return;
    }
    if (c && U32(c + 9) >= 0x100 && U32(c + 9) < 0x4000) g_offPlayerCrouch = U32(c + 9);
    if (!HookFunction(eye, (void*)Hook_GetPlayerEye, (void**)&Real_GetPlayerEye)) {
        Real_GetPlayerEye = nullptr;
        Log("Engine: dual: GetPlayerEyeTransform NOT hooked - Total Overdose keeps both guns in the dominant hand");
        return;
    }
    g_dual.hooked = true;
    Log("Engine: dual: GetPlayerEyeTransform hooked (+%llx; crouch %s player+%x) - dual wield available (Total "
        "Overdose: one gun in each hand)", (unsigned long long)(eye - g_base), g_offPlayerCrouch ? "at" : "NOT found, 0 at",
        g_offPlayerCrouch);
}

// --- Mods' menus as floating panels: does the window read the right mouse button? ---------------
// InputPressed / InputDown / InputReleased (Ui* table bindings) are watched while a menu is shown:
// when its script asks for "rmb" or "grab" (the right button by default), a grip on its content stays
// the window's right click (vr.cpp, GripTarget); a window that never asked can be moved by a grip
// anywhere on it. The key name is the binding's first Lua argument, read from the Lua 5.1 stack
// (lua_State: top at g_offLuaTop, base right after; a value: 16 bytes, type 4 = string at +8;
// TString: type at +8, length at +16, characters at +24), checked against the binding's own
// argument count - and under SEH: any surprise reads as "not a string".
PFN_LuaFn Real_InputPressed = nullptr, Real_InputDown = nullptr, Real_InputReleased = nullptr;

bool LuaFirstArgString(void* args, char* out, size_t n)
{
    __try {
        BYTE* L = *(BYTE**)args;
        const int count = *(const int*)((const BYTE*)args + g_offLuaArgCount);
        if (!L || count < 1 || count > 16) return false;
        BYTE* top = *(BYTE**)(L + g_offLuaTop);
        BYTE* base = *(BYTE**)(L + g_offLuaTop + 8);
        if (!top || !base || top - base != (ptrdiff_t)count * 16) return false;
        if (*(const int*)(base + 8) != 4) return false;
        const BYTE* ts = *(BYTE**)base;
        if (!ts || ts[8] != 4) return false;
        const size_t len = *(const size_t*)(ts + 16);
        if (len == 0 || len >= n) return false;
        memcpy(out, ts + 24, len);
        out[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PanelInputQuery(void* ctx, void* args)
{
    ModPanelState& P = g_panel;
    if (!P.active || P.rmbAware || !ctx || !args) return;
    if (*(BYTE**)((BYTE*)ctx + g_offScriptUi) != P.ui) return;
    char name[16];
    if (!LuaFirstArgString(args, name, sizeof(name))) return;
    // (positive evidence that the reads work for this window: without it, a reader that never decodes
    // would make every window "never reads the right button" - EngineGetModPanel, rmbFree)
    P.argSeen = true;
    if (!P.argLogged) {
        P.argLogged = true;
        Log("Engine: mod panel: key names read from the window's input queries (first: '%s') - its right-button reads "
            "are known", name);
    }
    if (_stricmp(name, "rmb") != 0 && _stricmp(name, "grab") != 0) return;
    P.rmbAware = true;
    if (P.rmbN < 16 && !PanelRmbKnown(P.ui)) P.rmbUis[P.rmbN++] = P.ui;
    if (PanelLog())
        Log("Engine: mod panel: its window reads the right mouse button ('%s') - a grip on its content stays the right "
            "click (moved from its frame band or with the other hand)", name);
}

__int64 __fastcall Hook_InputPressed(void* ctx, void* args, void* a3, void* a4)
{
    PanelInputQuery(ctx, args);
    return Real_InputPressed(ctx, args, a3, a4);
}

__int64 __fastcall Hook_InputDown(void* ctx, void* args, void* a3, void* a4)
{
    PanelInputQuery(ctx, args);
    return Real_InputDown(ctx, args, a3, a4);
}

__int64 __fastcall Hook_InputReleased(void* ctx, void* args, void* a3, void* a4)
{
    PanelInputQuery(ctx, args);
    return Real_InputReleased(ctx, args, a3, a4);
}

// Mods' menus as floating panels (ModPanel). Needs the windows' pointer (InitModUiPointer) and the
// UI hooks; any check failing leaves those windows as before (head-locked canvas at modUiDistance).
void InitModPanel()
{
    if (!g_modUi.ready || !g_offRenderer || !g_offRendererProj || !g_offScriptUi || !g_offUiDepth || !g_uiReady) {
        Log("Engine: mods' menus as floating panels not available (the windows' pointer or the 2D renderer was not "
            "recognised) - shown as before, in front of the head");
        return;
    }
    // Script::draw resets the UiPush depth before the script's draw():
    //   mov rax,[rsi+ui] ; mov dword [rax+depth],0   (the only reset in the game)
    BYTE* reset = nullptr;
    int resets = 0;
    for (BYTE* p = g_text; p && (p = ScanIn(p, g_textSize - (p - g_text), "48 8B 86 ?? ?? 00 00 C7 80 ?? ?? 00 00 00 00 00 00"));
         ++p)
        if (U32(p + 3) == g_offScriptUi && U32(p + 9) == g_offUiDepth) {
            reset = p;
            ++resets;
        }
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = reset && resets == 1 ? RtlLookupFunctionEntry((DWORD64)reset, &imageBase, nullptr) : nullptr;
    BYTE* draw = rf ? (BYTE*)imageBase + rf->BeginAddress : nullptr;
    // ... it starts "mov [rsp+10h],rbx ; mov [rsp+18h],rbp", and gives the script its size first:
    //   mov rax,[rsi+ui] ; mov [rax+10h],r15d ; mov rcx,[rsi+ui] ; mov eax,[rsp+X] ; mov [rcx+14h],eax
    const bool drawOk = draw && reset - draw < 0x400 && Matches(draw, "48 89 5C 24 10 48 89 6C 24 18") &&
                        ScanIn(draw, reset - draw, "48 8B 86 ?? ?? 00 00 44 89 78 10 48 8B 8E ?? ?? 00 00 8B 84 24 ?? ?? 00 00 89 41 14");
    // Every Ui* draw builds its MVP from the renderer's copy: lea rcx,[r8+proj] ; add r8,proj+40h
    int mvp = 0;
    for (BYTE* p = g_text; p && (p = ScanIn(p, g_textSize - (p - g_text), "49 8D 48 ?? 49 83 C0 ??")); ++p)
        if (p[3] == g_offRendererProj && p[7] == g_offRendererProj + 0x40) ++mvp;
    if (!drawOk || mvp < 5) {
        Log("Engine: mods' menus as floating panels not available (script draw %p, %d depth resets, %d MVP sites) - shown "
            "as before, in front of the head", draw, resets, mvp);
        return;
    }
    // The clip function (UiClipRect's first call): mov rax,rsp ; mov [rax+8],rbx ; push rdi ; sub rsp,X ;
    // ... movss xmm1,[rdi+clip] ; ucomiss xmm1,xmm11 ... mov rcx,[rax+renderer] ; mov r10,[rcx] ; ... call [r10+slot]
    BYTE* clipRect = FindUiBinding("UiClipRect");
    BYTE* clip = nullptr;
    for (BYTE* q = clipRect; q && q < clipRect + 0x100 && !clip; ++q)
        if (q[0] == 0xE8) {
            BYTE* t = RelTarget(q + 5, q + 1);
            if (t >= g_text && t + 0x220 <= g_text + g_textSize && Matches(t, "48 8B C4 48 89 58 08 57 48 81 EC ?? ?? 00 00"))
                clip = t;
        }
    BYTE* rect = clip ? ScanIn(clip, 0x180, "F3 0F 10 8F ?? ?? 00 00 41 0F 2E CB") : nullptr;
    BYTE* call = clip ? ScanIn(clip, 0x200, "48 8B 48 ?? 4C 8B 11") : nullptr;
    BYTE* slot = call ? ScanIn(call, 0x20, "41 FF 52 ??") : nullptr;
    bool clipOk = rect && call && slot && call[3] == g_offRenderer && U32(rect + 4) < 0x1000 && slot[3] >= 0x10;
    if (!HookFunction(draw, (void*)Hook_ScriptDraw, (void**)&Real_ScriptDraw)) return;
    if (clipOk) {
        g_offUiClip = U32(rect + 4);
        g_scissorSlot = slot[3];
        clipOk = HookFunction(clip, (void*)Hook_UiClip, (void**)&Real_UiClip);
        if (!clipOk) g_offUiClip = g_scissorSlot = 0;
    }
    if (!clipOk)
        Log("Engine: mod panel: the clip function was not recognised (%p %p %p %p) - a window clipped with UiWindow(w, h, "
            "true) / UiClipRect is clipped where it was (its content may be cut)", clipRect, clip, rect, slot);
    // The background blur's amount (PanelNoBlur): UiBackgroundBlur stores it - mov rdi,[rcx+ui] ; ... ;
    // call ArgFloat ; maxss ; minss xmm0,[max] ; movss [rdi+blur],xmm0
    BYTE* blurFn = FindUiBinding("UiBackgroundBlur");
    BYTE* blurUi = blurFn ? ScanIn(blurFn, 0x30, "48 8B B9 ?? ?? 00 00") : nullptr;
    BYTE* blurSt = blurFn ? ScanIn(blurFn, 0xC0, "F3 0F 5D 05 ?? ?? ?? ?? F3 0F 11 87 ?? ?? 00 00") : nullptr;
    bool blurOk = blurUi && blurSt && U32(blurUi + 3) == g_offScriptUi && U32(blurSt + 12) >= 0x100 &&
                  U32(blurSt + 12) < 0x1000 && U32(blurSt + 12) != g_offUiClip;
    if (blurOk) {
        g_offUiBlur = U32(blurSt + 12);
        blurOk = HookFunction(blurFn, (void*)Hook_UiBackgroundBlur, (void**)&Real_UiBackgroundBlur);
        if (!blurOk) g_offUiBlur = 0;
    }
    if (!blurOk)
        Log("Engine: mod panel: UiBackgroundBlur not recognised (%p %p %p) - a window drawing a blurred background is "
            "blurred at its old place (head-locked) and, clipped, may be cut", blurFn, blurUi, blurSt);
    // (the right button: the binding's first argument, read with the argument count and the Lua stack top)
    bool rmb = false;
    if (g_offLuaArgCount && g_offLuaTop) {
        BYTE* fp = FindUiBinding("InputPressed");
        BYTE* fd = FindUiBinding("InputDown");
        BYTE* fr = FindUiBinding("InputReleased");
        rmb = fp && fd && fr && HookFunction(fp, (void*)Hook_InputPressed, (void**)&Real_InputPressed) &&
              HookFunction(fd, (void*)Hook_InputDown, (void**)&Real_InputDown) &&
              HookFunction(fr, (void*)Hook_InputReleased, (void**)&Real_InputReleased);
    }
    g_panel.rmbHooks = rmb;
    g_panel.ready = true;
    Log("Engine: mod panel ready (script draw +%llx, renderer +%x/+%x, %d MVP sites; clip %s +%llx, rect ui+%x, scissor "
        "slot +%x; background blur %s ui+%x; right-button reads %s)",
        (unsigned long long)(draw - g_base), g_offRenderer, g_offRendererProj, mvp, clipOk ? "hooked" : "NOT hooked",
        clip ? (unsigned long long)(clip - g_base) : 0ull, g_offUiClip, g_scissorSlot, blurOk ? "left out at" : "NOT handled",
        g_offUiBlur, rmb ? "watched" : "not watched (a grip moves the panel from its frame band or with the other hand)");
}

// ---------------------------------------------------------------------------
// Measurement ([Debug] perfTrace, perf.h): the engine functions it hooks
// ---------------------------------------------------------------------------
// Each pattern must match exactly once in .text (Teardown 2.0.1: all unique), else that
// measurement is left out. Nothing is searched with perfTrace=0.
BYTE* ScanOnce(const char* pattern)
{
    BYTE* p = Scan(pattern);
    if (!p) return nullptr;
    return ScanIn(p + 1, g_textSize - (size_t)(p + 1 - g_text), pattern) ? nullptr : p;
}

// A string of .rdata standing alone ("\0name\0").
BYTE* FindRDataString(const char* s)
{
    const size_t len = strlen(s) + 1;
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp((const char*)sec->Name, ".rdata", 6) != 0) continue;
        BYTE* r = g_base + sec->VirtualAddress;
        for (size_t k = 1; k + len < sec->Misc.VirtualSize; ++k)
            if (r[k] == (BYTE)s[0] && r[k - 1] == 0 && memcmp(r + k, s, len) == 0) return r + k;
    }
    return nullptr;
}

// The dispatcher every Lua binding of the game goes through (2.0.1 +4a9050). Each binding's wrapper passes the
// implementation (r8), the binding's name (rdx) and its realm (ecx) to the dispatcher (r9 = the calling script,
// its arguments and results on the stack). Taken from three wrappers, which must agree; then the dispatcher must
// keep its 4 register arguments and call the implementation with the 2 stack ones: 6 arguments, as the hook
// passes them on (checked below).
BYTE* FindBindingDispatcher()
{
    BYTE* found = nullptr;
    for (const char* fname : { "GetCameraTransform", "DrawLine", "QueryRaycast" }) {
        BYTE* name = FindRDataString(fname);
        if (!name) return nullptr;
        BYTE* target = nullptr;
        for (size_t i = 7; i + 17 < g_textSize && !target; ++i) {
            BYTE* p = g_text + i;
            if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15 && p[-7] == 0x4C && p[-6] == 0x8D && p[-5] == 0x05 &&
                RelTarget(p + 7, p + 3) == name && p[7] == 0xB9 && p[12] == 0xE8)
                target = RelTarget(p + 17, p + 13);
        }
        if (!target || (found && target != found)) return nullptr;
        found = target;
    }
    if (!found || found < g_text || found + 0x400 > g_text + g_textSize || !ScanIn(found, 0x40, "49 8B F9 4D 8B F0 4C 8B EA 44 8B F9") ||
        !ScanIn(found, 0x400, "4C 8B 45 7F 48 8B 55 77 48 8B CF 41 FF D6"))
        return nullptr;
    return found;
}

// A qword equal to v in [lo, hi), 8-byte aligned, in readable committed memory.
BYTE* FindQword(BYTE* lo, BYTE* hi, uintptr_t v)
{
    lo = (BYTE*)(((uintptr_t)lo + 7) & ~(uintptr_t)7);
    for (BYTE* p = lo; p + 8 <= hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof(mi))) return nullptr;
        BYTE* end = std::min(hi, (BYTE*)mi.BaseAddress + mi.RegionSize);
        const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY;
        if (mi.State == MEM_COMMIT && (mi.Protect & rd) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            for (BYTE* q = p; q + 8 <= end; q += 8)
                if (*(const uintptr_t*)q == v) return q;
        p = end;
    }
    return nullptr;
}

// Registry setters (and SetPaused, UiSoundLoop...): not through the dispatcher, but a static list of
// { function, name, next } entries in .data (2.0.1: SetBool +4b7050 at entry +c43eb0). Found from the name:
// the entry holds a pointer to it, the function pointer just before. The first one is searched
// in all of .data, the others near it (the same list).
int FindCoreFunctions(Perf::Targets& t)
{
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE *data = nullptr, *dataEnd = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (strncmp((const char*)sec->Name, ".data", 5) == 0 && sec->Name[5] == 0) {
            data = g_base + sec->VirtualAddress;
            dataEnd = data + sec->Misc.VirtualSize;
        }
    if (!data) return 0;
    BYTE* anchor = nullptr;
    // (the registry setters first; then the other core functions that act - frame B mode 1 keeps the
    // frame-B tick of a mod that called one: script calls, events, input clearing, console commands,
    // and the engine helpers of the robots / radio links that draw lines, emit smoke, heat or move;
    // and the event readers: an event posted in frame A is readable in frame B only; and UiSoundLoop /
    // UiMute, one-frame requests used up at the next frame start, 0xffc30 / 0xff9e2, like PlayLoop -
    // a mod calling them from tick() must ask again in frame B or its loop pumps, its mute flickers;
    // and PauseMenuButton: every update clears "game.pausemenu" before the ticks (0x7ad25), so a mod
    // not ticked in frame B would lose its pause-menu button - the menu opens on frame B's list)
    for (const char* fname : { "SetBool", "SetInt", "SetFloat", "SetString", "SetColor", "ClearKey", "SetValue",
                               "SetValueInTable", "SetPaused", "ClientCall", "ServerCall", "PostEvent", "InputClear",
                               "Command", "RadiolinkDrawLinesCPP", "FxEmitSmokeCPP", "AddHeat", "SetGravity",
                               "RobotHoverGetUpCPP", "SetMapOrientation", "GetEventCount", "GetEvent", "UiSoundLoop",
                               "UiMute", "PauseMenuButton" }) {
        if (t.coreCount >= (int)(sizeof(t.core) / sizeof(t.core[0]))) break;
        BYTE* name = FindRDataString(fname);
        if (!name) continue;
        BYTE* lo = anchor ? std::max(data + 8, anchor - 0x4000) : data + 8;
        BYTE* hi = anchor ? std::min(dataEnd - 8, anchor + 0x4000) : dataEnd - 8;
        auto inText = [](const BYTE* p) { return p >= g_text && p < g_text + g_textSize; };
        // (another table may hold the same name first, e.g. "ServerCall": the next qword is tried)
        BYTE *e = nullptr, *fn = nullptr;
        for (BYTE* from = lo; from < hi;) {
            BYTE* c = FindQword(from, hi, (uintptr_t)name);
            if (!c) break;
            BYTE* f = *(BYTE**)(c - 8);
            BYTE* next = *(BYTE**)(c + 8);
            // (the next entry, if any, is one too: its function in .text)
            if (inText(f) && (!next || (next >= data && next + 24 <= dataEnd && inText(*(BYTE**)next)))) {
                e = c;
                fn = f;
                break;
            }
            from = c + 8;
        }
        if (!e) continue;
        t.core[t.coreCount++] = { (const char*)name, fn };
        if (!anchor) anchor = e;
    }
    return t.coreCount;
}

// The first address of the teardown.exe function p is in (its primary unwind entry), or nullptr.
BYTE* FunctionStart(BYTE* p)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)p, &imageBase, nullptr);
    if (!f || (BYTE*)imageBase != g_base) return nullptr;
    for (int k = 0; k < 8; ++k) {
        const BYTE* u = g_base + f->UnwindData;
        if (!((u[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        f = (PRUNTIME_FUNCTION)(u + 4 + ((u[2] + 1) & ~1) * 2);
    }
    return g_base + f->BeginAddress;
}

// frameb.h Targets::rightEyeOffset: the right eye from the left one for this pair, as Hook_CamSet
// composes it (g_eyeOffset, its separation scaled by g_curK: the map's, a mod camera's).
bool FrameBRightEyeOffset(float* f)
{
    if (!g_vrActive) return false;
    const Vec3 p = g_eyeOffset.p * g_curK;
    f[0] = p.x;
    f[1] = p.y;
    f[2] = p.z;
    f[3] = g_eyeOffset.q.x;
    f[4] = g_eyeOffset.q.y;
    f[5] = g_eyeOffset.q.z;
    f[6] = g_eyeOffset.q.w;
    return true;
}

// Frame B work ([Performance] frameBMode, frameb.h): what frame B replays or records. Each pattern
// also holds the offsets frameb.cpp uses (Teardown 2.0.1); one not found leaves mode 2 unavailable.
// See docs/REVERSE_ENGINEERING.md, "Image B allegee".
void FindFrameBTargets(FrameB::Targets& t, const Perf::Targets& perf)
{
    // Renderer::addLine(r, a, b, color0, color1 [stack], flag [stack]): ... mov eax,[rcx+23B4h]
    // (capacity) ... cmp eax,[rcx+23B0h] (count); entries of 3Ch: a, b +0Ch, color0 +18h, color1 +28h, flag +38h
    t.lineAdd = ScanOnce("4C 8B DC 53 48 83 EC 70 8B 42 08 48 8B D9 F2 0F 10 02 89 44 24 38 41 8B 40 08 89 44 24 44 "
                         "48 8B 84 24 A0 00 00 00 F2 0F 11 44 24 30 F2 41 0F 10 00 F2 0F 11 44 24 3C 0F 10 10 "
                         "0F B6 84 24 A8 00 00 00 41 0F 10 01 88 44 24 68 8B 81 B4 23 00 00 0F 11 44 24 48 "
                         "0F 11 54 24 20 0F 11 54 24 58 3B 81 B0 23 00 00");
    // Renderer::addSprite(r, entry): lea rdi,[rcx+23C0h] ; mov rsi,rdx ; mov edx,[rdi+4] ; ... imul r8,rax,58h
    t.spriteAdd = ScanOnce("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8D B9 C0 23 00 00 48 8B F2 8B 57 04");
    if (t.spriteAdd && !ScanIn(t.spriteAdd, 0x60, "48 63 07 4C 6B C0 58")) t.spriteAdd = nullptr;
    // Renderer::addDebugLine(r, a, b, color): ... call packColor ; mov ebp,[rdi+1918h] ; lea rbx,[rdi+18F8h]
    t.debugLineAdd = ScanOnce("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B F9 49 8B F0 "
                              "49 8B C9 4C 8B FA E8 ?? ?? ?? ?? 8B AF 18 19 00 00 48 8D 9F F8 18 00 00");
    // Renderer::addHighlight(r, shape, alpha): skipped while game+8A0h is set ; map at r+23F8h ; alpha (xmm2)
    t.highlightAdd = ScanOnce("48 83 EC 48 48 8B 05 ?? ?? ?? ?? 0F 29 74 24 30 0F 28 F2 80 B8 A0 08 00 00 00 75 ?? "
                              "48 89 54 24 20 4C 8D 44 24 68 48 8D 54 24 20 48 81 C1 F8 23 00 00 E8");
    // Renderer::addOutline(r, shape, rgba): the same, map at r+2410h
    t.outlineAdd = ScanOnce("48 83 EC 58 48 8B 05 ?? ?? ?? ?? 80 B8 A0 08 00 00 00 75 ?? 66 0F 6F 05 ?? ?? ?? ?? 48 81 C1 10 24 00 00");
    // Renderer::setDof(r, a, b, c): [r+A28h] +0 / +4 / +8 = xmm1-3, +0Ch = 0 ; byte [r+23F0h] = 1
    t.dofSet = ScanOnce("48 8B 81 28 0A 00 00 F3 0F 11 08 48 8B 81 28 0A 00 00 F3 0F 11 50 04 48 8B 81 28 0A 00 00 "
                        "F3 0F 11 58 08 48 8B 81 28 0A 00 00 C6 40 0C 00 C6 81 F0 23 00 00 01 C3");
    // SetLowHealthBlurThreshold's setter: movss [rcx+23F4h],xmm1 ; ret
    t.blurOk = ScanOnce("F3 0F 11 89 F4 23 00 00 C3") != nullptr;
    // The light lists, cleared in Game::update only: two lists of 0B10h bytes from r+2428h (count at +0),
    // called as  mov rcx,[rsi+48h] (renderer) ; call  right after  lea rcx,[rsp+30h] ; call
    BYTE* lights = ScanOnce("48 8D 81 28 24 00 00 48 8D 88 20 16 00 00 48 3B C1 74 ?? 33 D2 66 66 66 0F 1F 84 00 00 00 00 00 "
                            "89 10 48 05 10 0B 00 00 48 3B C1 75 F3 C3");
    BYTE* lightsCall = ScanOnce("48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8B 4E 48 E8 ?? ?? ?? ??");
    t.lightsOk = lights && lightsCall && RelTarget(lightsCall + 19, lightsCall + 15) == lights;
    // The real frame time game+208h, as Game::update's end reads it: mov r8,[rsi+38h] (input) ;
    // movss xmm1,[rsi+208h] ; mov rcx,[rsi+80h] (HUD script) ; call Script::tick ; cmp dword [rsi+8],6 ;
    // jne ; ... mov rcx,[rsi+98h] (game mode script) ; call Script::tick ; mov byte [rsi+850h],0 ;
    // xor ebx,ebx ; cmp [rsi+0B8h],ebx ; jle ; then for each player: mov rcx,[rsi+0C0h] ;
    // movss xmm1,[rsi+208h] ; mov rcx,[rdi+rcx] ; call trail  (mode 2 gives them the pair's real time)
    if (BYTE* p = ScanOnce("4C 8B 46 38 F3 0F 10 8E 08 02 00 00 48 8B 8E 80 00 00 00 E8 ?? ?? ?? ?? 83 7E 08 06 75 ?? "
                           "4C 8B 46 38 F3 0F 10 8E 08 02 00 00 48 8B 8E 98 00 00 00 E8 ?? ?? ?? ?? C6 86 50 08 00 00 00 "
                           "33 DB 39 9E B8 00 00 00 7E")) {
        BYTE* tick = RelTarget(p + 24, p + 20);
        BYTE* q = ScanIn(p + 70, 0x40, "48 8B 8E C0 00 00 00 F3 0F 10 8E 08 02 00 00 48 8B 0C 0F E8");
        t.realTimeOk = q && tick == RelTarget(p + 54, p + 50) && Matches(tick, "48 8B C4 48 89 58 18 F3 0F 11 48 10");
    }
    // Script events (PostEvent / GetEvent): two buffers, posted into [mgr+0], read from [mgr+4]
    // (mgr = [game+158h]); Game::frame swaps them at each frame start, the swap clearing the new
    // write buffer: mov edx,[rcx+4] ; mov eax,[rcx] ; mov [rcx+4],eax ; mov [rcx],edx ; jmp clear,
    // called as  mov rcx,[g_game] ; mov rcx,[rcx+158h] ; call ; mov rcx,[r12+160h]
    {
        BYTE* swap = ScanOnce("8B 51 04 8B 01 89 41 04 89 11 E9");
        BYTE* call = ScanOnce("48 8B 0D ?? ?? ?? ?? 48 8B 89 58 01 00 00 E8 ?? ?? ?? ?? 49 8B 8C 24 60 01 00 00");
        if (swap && call && RelTarget(call + 19, call + 15) == swap) t.eventSwap = swap;
    }
    // Game::frame, time scale: mov eax,[r12+1FCh] ; mov [r12+1F8h],eax ; lea rsi,[r12+200h] ;
    // mov rax,[r12+160h] ; cmp dword [rax],1 ; jne ; mov dword [r12+1FCh],1.0
    t.timeScaleOk = ScanOnce("41 8B 84 24 FC 01 00 00 41 89 84 24 F8 01 00 00 49 8D B4 24 00 02 00 00 49 8B 84 24 60 01 00 00 "
                             "83 38 01 75 ?? 41 C7 84 24 FC 01 00 00 00 00 80 3F") != nullptr;
    // Sound: Game::frame: mov rcx,[r12+58h] ; call SoundManager::update ; cmp dword [r12+8],6. In it the
    // loops: cmp dword [r14+58h],0 ; jle ; ... mov rcx,[r14+60h] ; mov rcx,[rbx+rcx] ; call Loop::update ;
    // ... cmp ebp,[r14+58h] ; jl. Loop::update reads the request at +88h first.
    BYTE* sm = ScanOnce("49 8B 4C 24 58 E8 ?? ?? ?? ?? 41 83 7C 24 08 06");
    BYTE* lw = ScanOnce("41 83 7E 58 00 7E ?? 48 8B DD 66 0F 1F 44 00 00 49 8B 4E 60 48 8B 0C 0B E8 ?? ?? ?? ?? FF C5 "
                        "48 8D 5B 08 41 3B 6E 58 7C");
    t.soundOk = sm && lw && FunctionStart(lw) == RelTarget(sm + 10, sm + 6) &&
                Matches(RelTarget(lw + 29, lw + 25), "48 89 5C 24 08 57 48 83 EC 40 F3 0F 10 91 88 00 00 00");
    // Camera requests (camera = game+60h): SetCameraFov writes +2C8h; SetCameraTransform its pose at
    // +264h, its fov at +2C4h, the flag +2CCh; SaveCameraOverrideTransform +261h; AttachCameraTo the
    // entity at +2D0h and +2D8h; SetCameraOffsetTransform's setter the pose at +1B0h and the flag +1CCh.
    BYTE* fov = FindLuaBinding("SetCameraFov");
    BYTE* ct = FindLuaBinding("SetCameraTransform");
    BYTE* sv = FindLuaBinding("SaveCameraOverrideTransform");
    BYTE* at = FindLuaBinding("AttachCameraTo");
    t.cameraOk = g_offGamePlayer == 0x60 && fov && ScanIn(fov, 0x60, "48 8B 48 60 F3 0F 5D 05 ?? ?? ?? ?? F3 0F 11 81 C8 02 00 00") &&
                 ct && ScanIn(ct, 0x140, "48 8B 58 60") &&
                 ScanIn(ct, 0x140, "0F 11 8B 64 02 00 00 F2 0F 10 54 24 ?? F2 0F 11 93 74 02 00 00 8B 44 24 ?? 89 83 7C 02 00 00 "
                                   "F3 0F 11 83 C4 02 00 00 C6 83 CC 02 00 00 01") &&
                 sv && ScanIn(sv, 0x80, "48 8B 48 60 C6 81 61 02 00 00 01") &&
                 at && ScanIn(at, 0xa0, "48 8B 51 60 48 89 9A D0 02 00 00 48 8B 5C 24 ?? 88 82 D8 02 00 00") &&
                 ScanOnce("0F 10 02 0F 11 81 B0 01 00 00 F2 0F 10 4A 10 F2 0F 11 89 C0 01 00 00 8B 42 18 89 81 C8 01 00 00 "
                          "C6 81 CC 01 00 00 01 C3");
    // Registry (game+100h), from Game::update's tail (game.disableinput reset): mov rbx,[rax+100h] ;
    // lea rdx,["game.disableinput"] ; lea rcx,[rsp+20h] ; call String::String(str, text) ; xor r9d,r9d ;
    // xor r8d,r8d ; lea rdx,[rsp+20h] ; mov rcx,rbx ; call Registry::setBool ; lea rcx,[rsp+20h] ;
    // call String::~String. setBool writes "1" / "0" with Registry::setString(reg, key, value, sync), the
    // common path of SetBool / SetInt / SetFloat / SetString (not SetColor).
    if (BYTE* p = ScanOnce("40 53 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 8B 98 00 01 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 20 "
                           "E8 ?? ?? ?? ?? 90 45 33 C9 45 33 C0 48 8D 54 24 20 48 8B CB E8 ?? ?? ?? ?? 90 48 8D 4C 24 20 E8")) {
        const char* name = (const char*)RelTarget(p + 27, p + 23);
        BYTE* ctor = RelTarget(p + 37, p + 33);
        BYTE* setBool = RelTarget(p + 57, p + 53);
        BYTE* dtor = RelTarget(p + 68, p + 64);
        BYTE* q = ScanIn(setBool, 0x80, "44 0F B6 CE 4C 8D 44 24 20 48 8B D5 49 8B CE E8");
        BYTE* setString = q ? RelTarget(q + 20, q + 16) : nullptr;
        if (strcmp(name, "game.disableinput") == 0 &&
            Matches(ctor, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 85 D2 C6 01 00") &&
            Matches(dtor, "48 83 EC 28 80 79 1F 00 74 09 48 8B 09 E8") && setString &&
            Matches(setString, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 45 0F B6 F1 "
                               "49 8B F0 4C 8B FA 48 8B F9 48 8D 59 10")) {
            t.strCtor = ctor;
            t.strDtor = dtor;
            t.regSetString = setString;
        }
    }
    // Registry::clearKey(reg, key), from the ClearKey core function: mov rax,[game] ; lea rdx,[rsp+20h] ;
    // mov rcx,[rax+100h] ; call
    for (int i = 0; i < perf.coreCount; ++i) {
        if (strcmp(perf.core[i].name, "ClearKey") != 0) continue;
        BYTE* c = ScanIn(perf.core[i].fn, 0x100, "48 8D 54 24 20 48 8B 88 00 01 00 00 E8");
        BYTE* fn = c ? RelTarget(c + 17, c + 13) : nullptr;
        if (fn && Matches(fn, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B FA 48 8B E9 48 8D 59 10"))
            t.regClearKey = fn;
    }
    // The other one-frame sound requests SoundManager::update (sm = game+58h, r14 in it) uses up at
    // each frame start. The music keys: lea rdx,["game.music.volume" / "game.music.lowpass"] ;
    // lea rcx,[rsp+30h] ; call String ; nop ; lea rdx,[rsp+30h] ; mov rcx,rbx ; call Registry::clearKey
    // (read, applied, then cleared: 0xff61e, 0xff6cf).
    BYTE* smUpd = sm ? RelTarget(sm + 10, sm + 6) : nullptr;
    auto clearsKey = [&](const char* key) {
        BYTE* str = FindRDataString(key);
        if (!smUpd || !str || !t.regClearKey) return false;
        for (BYTE* p = smUpd; (p = ScanIn(p, (size_t)(smUpd + 0x800 - p), "48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 30 E8 ?? ?? ?? ?? 90 "
                                                                         "48 8D 54 24 30 48 8B CB E8")) != nullptr; ++p)
            if (RelTarget(p + 7, p + 3) == str && RelTarget(p + 31, p + 27) == t.regClearKey) return true;
        return false;
    };
    t.musicOk = clearsKey("game.music.volume") && clearsKey("game.music.lowpass");
    // GetLastSound: SoundManager::update moves the frame's loudest registered sound (+7680h volume,
    // +7684h position) to +7670h, then zeroes it (0xff911); the binding reads [game+58h]+7670h.
    if (BYTE* c = ScanOnce("F2 41 0F 10 86 84 76 00 00 F2 41 0F 11 86 74 76 00 00 41 8B 86 8C 76 00 00 41 89 86 7C 76 00 00 "
                           "41 8B 86 80 76 00 00 41 89 86 70 76 00 00")) {
        BYTE* gls = FindLuaBinding("GetLastSound");
        t.lastSoundOk = smUpd && FunctionStart(c) == smUpd && gls && ScanIn(gls, 0x30, "48 8B 48 58 F3 0F 10 89 70 76 00 00");
    }
    // UiMute (core function): [sm+7690h] = max(itself, amount), [sm+7694h] the same for the music;
    // SoundManager::update zeroes them after use: mov [r14+7690h],ebp ; mov [r14+7694h],ebp.
    for (int i = 0; i < perf.coreCount; ++i) {
        if (strcmp(perf.core[i].name, "UiMute") != 0) continue;
        BYTE* fn = perf.core[i].fn;
        t.muteOk = smUpd && ScanIn(fn, 0x100, "F3 0F 10 89 90 76 00 00 F3 0F 5F C8 F3 0F 11 89 90 76 00 00") &&
                   ScanIn(fn, 0x100, "F3 0F 10 89 94 76 00 00 F3 0F 5F C8 F3 0F 11 89 94 76 00 00") &&
                   ScanIn(smUpd, 0x800, "41 89 AE 90 76 00 00") && ScanIn(smUpd, 0x800, "41 89 AE 94 76 00 00");
    }
    t.entityOfHandle = EntityLookupReady() ? &EntityOfHandle : nullptr;
    t.nextFrameB = &EngineNextFrameFrozen;
    t.rightEyeOffset = &FrameBRightEyeOffset;
    auto rva = [](const void* q) { return q ? (unsigned long long)((const BYTE*)q - g_base) : 0ull; };
    Log("Engine: frame B targets: lines +%llx, sprites +%llx, debug lines +%llx, outlines +%llx, highlights +%llx, "
        "depth of field +%llx, blur %d, lights %d, real time %d, events +%llx, time scale %d, sound %d, music %d, "
        "last sound %d, mute %d, camera %d, registry +%llx / +%llx, strings +%llx / +%llx, handles %d",
        rva(t.lineAdd), rva(t.spriteAdd), rva(t.debugLineAdd), rva(t.outlineAdd), rva(t.highlightAdd), rva(t.dofSet),
        t.blurOk ? 1 : 0, t.lightsOk ? 1 : 0, t.realTimeOk ? 1 : 0, rva(t.eventSwap), t.timeScaleOk ? 1 : 0,
        t.soundOk ? 1 : 0, t.musicOk ? 1 : 0, t.lastSoundOk ? 1 : 0, t.muteOk ? 1 : 0, t.cameraOk ? 1 : 0,
        rva(t.regSetString), rva(t.regClearKey), rva(t.strCtor), rva(t.strDtor), t.entityOfHandle ? 1 : 0);
}

// [Performance] jobSpin (perf.h): where the job system sleeps. Every check must pass, else that part stays off.
void FindJobSpinTargets(Perf::Targets& t)
{
    // WorkerThread::run, idle: mov rax,[jm] ; mov ecx,-1 ; lock xadd [rax],ecx ; cmp ecx,1 ; jns ; mov rcx,[jm] ;
    // mov edx,INFINITE ; add rcx,100h ; call semWait ; nop ; mov rax,[jm] ; movzx eax,byte [rax+4] ; test al,al
    // (2.0.1 +5021e0; semWait +5111d0 has no other caller, but for an unreferenced copy of this wait at +502770)
    BYTE* w = ScanOnce("48 8B 05 ?? ?? ?? ?? B9 FF FF FF FF F0 0F C1 08 83 F9 01 79 ?? 48 8B 0D ?? ?? ?? ?? BA FF FF FF FF "
                       "48 81 C1 00 01 00 00 E8 ?? ?? ?? ?? 0F 1F 00 48 8B 05 ?? ?? ?? ?? 0F B6 40 04 84 C0");
    if (w) {
        BYTE* jm = RelTarget(w + 7, w + 3);
        BYTE* fn = RelTarget(w + 0x2D, w + 0x29);
        // semWait: sub rsp,28h ; mov rcx,[rcx] ; call [WaitForSingleObject] ; test eax,eax ; sete al ; add rsp,28h ; ret
        if (jm == RelTarget(w + 0x1C, w + 0x18) && jm == RelTarget(w + 0x37, w + 0x33) &&
            ScanIn(fn, 23, "48 83 EC 28 48 8B 09 FF 15 ?? ?? ?? ?? 85 C0 0F 94 C0 48 83 C4 28 C3") == fn) {
            t.jobManager = (BYTE**)jm;
            t.workerWait = fn;
            t.workerWaitRet = w + 0x2D;
        }
    }
    // Job::wait +100h, its wake of the extra worker before it blocks: mov edx,[tls] ; mov rax,gs:[58h] ; mov ecx,8 ;
    // mov rax,[rax+rdx*8] ; cmp dword [rcx+rax],-1 ; jne ; mov rcx,[jm] ; add rcx,150h ; call thunk ; mov rcx,[rdi] ;
    // mov edx,INFINITE ; call (the event's wait). The thunk: mov rcx,[rcx] ; jmp [SetEvent]
    BYTE* q = t.jobWait ? t.jobWait + 0x100 : nullptr;
    if (q && t.jobManager &&
        ScanIn(q, 58, "8B 15 ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 B9 08 00 00 00 48 8B 04 D0 83 3C 01 FF 75 ?? "
                      "48 8B 0D ?? ?? ?? ?? 48 81 C1 50 01 00 00 E8 ?? ?? ?? ?? 48 8B 0F BA FF FF FF FF E8") == q &&
        RelTarget(q + 0x25, q + 0x21) == (BYTE*)t.jobManager) {
        BYTE* thunk = RelTarget(q + 0x31, q + 0x2D);
        if (ScanIn(thunk, 6, "48 8B 09 48 FF 25") == thunk) {
            t.tlsIndex = (DWORD*)RelTarget(q + 6, q + 2);
            t.setEventThunk = thunk;
        }
    }
}

void InitPerfTrace()
{
    // ([Performance] frameBMode 1 or 2, or the control file TeardownVR_perf.txt: the census hooks and
    // the frame-B module; otherwise only perfTrace's own)
    const bool frameB = FrameB::HooksWanted();
    // ([Performance] jobSpin: the job system's waits, perf.h - found and hooked without perfTrace too)
    const bool spin = Perf::SpinWanted();
    if (!g_perfTrace && !frameB && !spin) return;
    Perf::Targets t;
    t.frameB = frameB;
    if (g_perfTrace >= 1) {
        // Job::wait: push rbx ; sub rsp,20h ; mov eax,[rcx+8] ; mov rbx,rcx ; cmp eax,4 ; je ; ...
        t.jobWait = ScanOnce("40 53 48 83 EC 20 8B 41 08 48 8B D9 83 F8 04 0F 84 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10");
        // TFenceD3D12::waitValueOnCpu: completed value (vtable +40h) below the one asked -> event, wait
        t.fenceWait = ScanOnce("48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 8B FA 48 8B 09 48 8B 01 FF 50 40 48 3B C7 73");
        // Profiler::add: skipped while paused (game+1E8h); slot += seconds x 1000 (12-byte slots at +40h)
        t.profAdd = ScanOnce("48 8B 05 ?? ?? ?? ?? 80 B8 E8 01 00 00 00 75 1B F3 0F 59 15 ?? ?? ?? ?? 48 63 C2 48 8D 14 40 "
                             "F3 0F 58 54 91 40 F3 0F 11 54 91 40 C3");
        // The swap chain's frame-latency wait (TRendererD3D12::endRender, after the Present): mov rcx,[rcx+60h]
        // (the waitable object) ; test ; je ret ; mov edx,INFINITE ; jmp [WaitForSingleObject]
        t.displayWait = ScanOnce("48 8B 49 60 48 85 C9 74 0C BA FF FF FF FF 48 FF 25");
        // ([Performance] perfDetail, perf.cpp) Job::run: push rbx ; sub rsp,20h ; mov rax,[rcx] ; mov rbx,rcx ;
        // call [rax+8] (the job's body) ; mov eax,4 ; xchg [rbx+8],eax (done)
        t.jobRun = ScanOnce("40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 08 B8 04 00 00 00 87 43 08");
        // TAsyncDataUploaderD3D12::submit: its staging buffer = [rcx+48h] + [rcx+0C0h] x 38h ; state +30h == 1
        // (data staged) -> close, ExecuteCommandLists on the uploader's COPY queue, Signal
        t.uploadSubmit = ScanOnce("48 89 5C 24 20 55 57 41 57 48 83 EC 30 48 63 81 C0 00 00 00 4D 8B F8 48 6B F8 38 "
                                  "8B EA 48 8B D9 48 03 79 48 83 7F 30 01 0F 85");
        // ([Performance] perfDetail 8, perf.cpp "GPU time per pass": searched only when asked for)
        if (Perf::PassesWanted()) {
            // TRendererD3D12::setGraphicsProgram (vtable +190): program null -> false ; [rcx+1170h] = program ; its root
            // signature [rdx+78h] unless already [rcx+1178h]: mov rcx,[rcx+0E40h] (the frame's command list, read at +4b) ;
            // call [rax+0F0h] (SetGraphicsRootSignature)
            t.setGfxProgram = ScanOnce("48 89 74 24 10 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 75 0D 32 C0 48 8B 74 24 38 "
                                       "48 83 C4 20 5F C3 48 89 5C 24 30 48 39 B1 70 11 00 00 74 07 48 89 B1 70 11 00 00 "
                                       "48 8B 52 78 48 39 91 78 11 00 00 0F 84 ?? ?? ?? ?? 48 8B 89 40 0E 00 00 48 8B 01 "
                                       "FF 90 F0 00 00 00");
            // setComputeProgram (vtable +198, through a jmp): [rcx+1250h] = program, root signature [rdx+68h] /
            // [rcx+1258h], mov rcx,[rcx+0E40h] (read at +46), call [rax+0E8h] (SetComputeRootSignature)
            t.setCsProgram = ScanOnce("48 89 74 24 10 57 48 83 EC 30 48 8B F2 48 8B F9 48 85 D2 75 0D 32 C0 48 8B 74 24 48 "
                                      "48 83 C4 30 5F C3 48 39 B1 50 12 00 00 74 07 48 89 B1 50 12 00 00 48 8B 52 68 "
                                      "48 39 91 58 12 00 00 0F 84 ?? ?? ?? ?? 48 8B 89 40 0E 00 00 48 89 5C 24 40 48 8B 01 "
                                      "FF 90 E8 00 00 00");
            // beginRender (InitLazyBegin's pattern): mov byte [rcx+0CCh],1 - "in a frame" (read at +17), until endRender
            // has submitted the list
            t.beginRender = ScanOnce("48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 30 48 8B F9 C6 81 CC 00 00 00 01 "
                                     "E8 ?? ?? ?? ?? 48 89 87 70 0F 00 00 48 63 87 A8 0D 00 00 48 8D 1C 40 48 C1 E3 04 "
                                     "48 03 9F F8 0E 00 00 48 8B 8F 58 0E 00 00 48 8B 01 FF 50 40 48 8B 53 08 48 3B C2 "
                                     "73 0C 48 8D 8F 58 0E 00 00 E8");
            // The renderer's voxel textures (Renderer::render +88841, mov rcx,r15): for each texture in the frame's
            // culling list ([[rcx+0A20h]+90h], count +88h) with new data (+39h): upload; then the "voxtexturemips"
            // compute for those with new mips
            t.voxelTextures = ScanOnce("48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC "
                                       "80 00 00 00 4C 8B F9 48 8B 81 20 0A 00 00 48 8B 98 90 00 00 00 48 63 80 88 00 00 00 "
                                       "48 8D 34 C3 48 3B DE 74 ?? 48 8B 3B 80 7F 39 00 74 ?? 48 8B CF E8");
            // ShadowVolume's GPU part (Renderer::render +88cd1, through a jmp): wait for its ShadowTask jobs (+418h,
            // tasks +420h), then the "shadowmips" compute (+3ECh on and +8 != 1) or the uploads
            t.shadowVolume = ScanOnce("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 38 FC FF FF 48 81 EC C8 04 00 00 "
                                      "4C 8B F9 45 33 ED 44 89 AD 20 04 00 00 44 38 A9 EC 03 00 00 74 06 83 79 08 01 75 ?? "
                                      "44 39 A9 20 04 00 00 74 ?? 48 8B 89 18 04 00 00 E8");
        }
    }
    if (spin) {
        if (!t.jobWait)
            t.jobWait = ScanOnce("40 53 48 83 EC 20 8B 41 08 48 8B D9 83 F8 04 0F 84 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10");
        FindJobSpinTargets(t);
    }
    if (g_perfTrace >= 2 || frameB) {
        // A script entity's tick (its only caller: Scene::update's loop over the scripts):
        // server script [rcx+48h], then the client script [rcx+40h], each timed by the profiler.
        t.modTick = ScanOnce("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 0F 29 74 24 40 49 8B F0 0F 28 F1 48 8B F9 "
                             "48 8B 41 48 48 85 C0");
        // Its script path ("Single player script disabled: %s"): lea rdi,[rcx+path] ; mov rdx,rdi ;
        // mov rcx,[rax+mods] ; call ; test rax,rax ; je ; mov r8b,1 ; lea rdx,["local-"]
        if (BYTE* p = ScanOnce("48 8D B9 ?? ?? 00 00 48 8B D7 48 8B 88 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 41 B0 01 48 8D 15"))
            t.entPath = U32(p + 3);
        t.dispatcher = FindBindingDispatcher();
        FindCoreFunctions(t);
    }
    // (frame B's targets are searched before any hook is installed: some patterns start functions)
    FrameB::Targets fb;
    if (frameB) FindFrameBTargets(fb, t);
    Perf::InstallHooks(t);
    g_perfCensus = Perf::Active();
    if (frameB) g_frameBOn = FrameB::Init(fb);
}

// Lazy beginFrame ([Performance] lazyBeginFrame, perf.h): the kind of engine frame starting, as its
// TRendererD3D12::beginRender sees it (Game::frame +71f84, before Game::update): 1 = the left-eye frame A
// of a stereo pair (the frame before was a right-eye frame B, and no frame B is asked for yet), 2 = a
// right-eye frame B (asked for by the frame A just before), 0 = anything else (menus, loading, mono,
// alternate eyes).
int EngineFrameKindAtStart()
{
    if (!g_ready || !g_vrActive || Cfg().stereoMode != 0) return 0;
    if (g_rightPending) return 2;
    return g_lastWasFrozen ? 1 : 0;
}

// pauseWide (WideState): the game's level state and pause flag. Game::frame -
//   cmp dword [r12+state],6 ; jne ; cmp byte [r12+paused],0 ; jne ; call                    (2.0.1 +729cd)
// - and Game::update's pause test, which ends the update before any script's tick while a single-player game is
// paused (so our Lua asks for no eye: the frame goes to the virtual screen) -
//   cmp byte [rsi+paused],0 ; je ; mov rax,[rsi+160h] ; cmp dword [rax],1 ; jne ; cmp dword [rax+4],0 ; je (end)
//                                                                                             (2.0.1 +7acec)
// Both unique in .text, the same pause field in both.
void InitPauseWide()
{
    BYTE* f = ScanOnce("41 83 7C 24 ?? 06 75 ?? 41 80 BC 24 ?? ?? 00 00 00 75 ?? E8");
    BYTE* u = ScanOnce("80 BE ?? ?? 00 00 00 74 ?? 48 8B 86 ?? ?? 00 00 83 38 01 75 ?? 83 78 04 00 0F 84");
    if (!f || !u || !g_uiReady || U32(f + 12) != U32(u + 2) || U32(f + 12) >= 0x1000 || f[4] == 0 || f[4] >= 0x80) {
        Log("Engine: the game's pause not recognised (%p %p, UI camera %d) - a paused level is shown as the game lays it "
            "out (the virtual screen has the VR image's shape)", f, u, g_uiReady ? 1 : 0);
        return;
    }
    g_wide.offState = f[4];
    g_wide.offPaused = U32(f + 12);
    g_wide.ready = true;
    Log("Engine: paused level on a 16:9 virtual screen (state game+%x = 6, paused game+%x, UI camera game+%x; "
        "pauseWide %d)", g_wide.offState, g_wide.offPaused, g_offUiCam, Cfg().pauseWide ? 1 : 0);
}

// End of a frame A's update (Hook_Update; this frame's UI camera is already built, Game::frame +71e94, and the
// drawing comes after): a paused level that the headset shows on the virtual screen is drawn for a 16:9 screen
// (WideState). Not when our Lua asked for an eye in this update (a multiplayer game keeps ticking while paused: its
// pause menu stays on the HUD, in stereo), nor in cinema mode (forceScreen: the whole game on the screen keeps one
// shape), nor with alternate eyes. Only for a window narrower than 16:9 (the headset's, nearly square): a wider one
// already has a 1080-tall canvas and a screen of its own shape - forcing 1920 x 1080 there would stretch it sideways.
void WideDecide(BYTE* g)
{
    bool wide = false;
    float gameCw = 0, gameCh = 0;   // the canvas the game built (logged)
    const int W = *(const int*)g, H = *(const int*)(g + 4);
    if (g_wide.ready && g_uiReady && g_ready && Cfg().pauseWide && g_wide.screenLive && Cfg().stereoMode == 0 &&
        !Cfg().forceScreen && g_requestsThisUpdate == 0 && *(const int*)(g + g_wide.offState) == 6 &&
        g[g_wide.offPaused] && W > 0 && H > 0 && (int64_t)W * 9 < (int64_t)H * 16) {
        float* P = (float*)(g + g_offUiCam);   // pixels -> clip space
        float* V = P + 16;                     // canvas -> pixels
        // (this frame's camera as the game built it: Camera::setOrtho over W x H, one scale both ways, no shift)
        if (std::abs(P[0] * W - 2.0f) < 1e-3f && std::abs(P[5] * H + 2.0f) < 1e-3f && V[0] > 0 &&
            std::abs(V[5] - V[0]) <= 1e-4f * V[0] && V[12] == 0 && V[13] == 0 && V[15] == 1) {
            gameCw = W / V[0];
            gameCh = H / V[5];
            V[0] = W / 1920.0f;   // canvas 1920 x 1080 over the whole window
            V[5] = H / 1080.0f;
            wide = true;
        }
    }
    g_wide.frame = wide ? g_frameCounter : 0;
    if (wide == g_wide.was) return;
    g_wide.was = wide;
    g_wide.resetHistory = true;   // (Hook_Render: no TAA blend across the switch)
    if (++g_wide.switches > 40) return;
    if (wide)
        Log("Engine: paused level drawn for a 16:9 screen - 2D canvas 1920 x 1080 (the game's: %.0f x %.0f) and the "
            "game's 3D view at 16:9, both stretched over the %d x %d window (pauseWide)", gameCw, gameCh, W, H);
    else
        Log("Engine: 16:9 pause over - 2D canvas and 3D view as the game builds them");
    if (g_wide.switches == 40) Log("Engine: (16:9 pause: no more lines about it)");
}

// TRendererD3D12::beginRender and endRender for perf.cpp's lazy beginFrame (searched only with
// lazyBeginFrame >= 1 in the ini or a "lazyBeginFrame=N" line in the control file TeardownVR_perf.txt,
// which FrameB::Init has just read in InitPerfTrace: a control file without that line, as every frame B /
// frame latency test writes, leaves the renderer's vtable alone). Both unique in .text (Teardown 2.0.1
// +5b35a0 / +5b6e50); beginRender's pattern also holds the offsets perf.cpp reads (at +24 the frame's
// thread, +2b the context index, +3a the contexts, +41 the fence):
//   beginRender: marks the renderer as in a frame (+0CCh), stores this thread's id (+0F70h), picks the current
//   context (index +0DA8h into the 30h-byte contexts at +0EF8h) and waits on the fence (+0E58h) for that
//   context's last value; endRender: gets the swap chain's (+0E70h) back buffer (patterns below).
void InitLazyBegin()
{
    if (Cfg().lazyBeginFrame < 1 && FrameB::FileLazyBegin() < 0) return;
    BYTE* begin = ScanOnce("48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 30 48 8B F9 C6 81 CC 00 00 00 01 E8 ?? ?? ?? ?? "
                           "48 89 87 70 0F 00 00 48 63 87 A8 0D 00 00 48 8D 1C 40 48 C1 E3 04 48 03 9F F8 0E 00 00 "
                           "48 8B 8F 58 0E 00 00 48 8B 01 FF 50 40 48 8B 53 08 48 3B C2 73 0C 48 8D 8F 58 0E 00 00 E8");
    BYTE* end = ScanOnce("48 89 5C 24 18 55 56 57 41 56 41 57 48 83 EC 50 48 8B D9 C6 84 24 84 00 00 00 00 48 81 C1 70 0E 00 00 E8");
    Perf::LazyBeginInstall(begin, end, &EngineFrameKindAtStart);
}

// Floats the game's code can read through a rip-relative operand: allocated within 2 GB of its image
// (searched downward from its base, one allocation granule at a time, as MinHook places its trampolines).
float* AllocNearGame(size_t count)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity, base = (uintptr_t)g_base;
    const uintptr_t lowest = base > 0x70000000ull + gran ? base - 0x70000000ull : gran;
    for (uintptr_t a = (base & ~(gran - 1)) - gran; a >= lowest && a < base; a -= gran)
        if (void* p = VirtualAlloc((void*)a, count * sizeof(float), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return (float*)p;
    return nullptr;
}

// Points the rip-relative operand of the 8-byte SSE instruction at insn (F3 0F op modrm, disp32) to target.
bool RedirectOperand(BYTE* insn, const float* target)
{
    const long long d = (long long)((const BYTE*)target - (insn + 8));
    if (d < INT_MIN || d > INT_MAX) return false;
    const int32_t d32 = (int32_t)d;
    DWORD old;
    if (!VirtualProtect(insn + 4, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(insn + 4, &d32, 4);
    VirtualProtect(insn + 4, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), insn, 8);
    return true;
}

// grabKeepDistance (see GrabControls): the game's grab and its per-frame clamp, both unique in .text.
//   Player::grab (2.1 +ba1e4): sets the grab distance (+7B0h) to the hit distance (+7ACh) minus 0.2, at least 1.5;
//   Player::update (2.1 +c2c28): while something is grabbed (+788h), adds the mouse wheel's notches x 0.25 to it
//   and clamps it to 1.5 .. 3.0 (patterns below).
void InitGrabDistance()
{
    BYTE* g = ScanOnce("F3 0F 11 B6 ?? ?? 00 00 F3 0F 5C 35 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? F3 0F 5F C6 "
                       "F3 0F 11 86 ?? ?? 00 00");
    BYTE* w = ScanOnce("F3 0F 10 35 ?? ?? ?? ?? 48 83 BE ?? ?? 00 00 00 74 ?? 49 8B CF E8 ?? ?? ?? ?? 66 0F 6E C0 "
                       "0F 5B C0 F3 0F 59 05 ?? ?? ?? ?? F3 0F 58 86 ?? ?? 00 00 F3 0F 5F 05 ?? ?? ?? ?? F3 0F 5D C6 "
                       "F3 0F 11 86 ?? ?? 00 00");
    auto value = [](BYTE* end, BYTE* rel) {
        float v;
        memcpy(&v, RelTarget(end, rel), 4);
        return v;
    };
    // (the same wanted-distance field in both; the constants as read: 0.2, 1.5, 1.5 and the 3 m maximum)
    const bool ok = g && w && g_offPlayerList && U32(g + 32) == U32(w + 45) && U32(w + 45) == U32(w + 65) &&
                    value(g + 16, g + 12) == 0.2f && value(g + 24, g + 20) == 1.5f && value(w + 57, w + 53) == 1.5f &&
                    value(w + 8, w + 4) >= 2.0f && value(w + 8, w + 4) <= 10.0f;
    if (!ok) {
        Log("Engine: the game's grab distance not recognised (grab %p, update %p) - objects grabbed with the dominant "
            "grip keep the game's rule (1.5 m from the eyes at least, the stick moves them in 25 cm steps)", g, w);
        return;
    }
    float* k = AllocNearGame(2);
    if (!k) {
        Log("Engine: no memory near the game's code - the dominant grip's grab keeps the game's rule");
        return;
    }
    k[0] = 0.2f;   // (the game's own values until GrabControls runs)
    k[1] = 1.5f;
    if (!RedirectOperand(g + 8, k) || !RedirectOperand(g + 16, k + 1) || !RedirectOperand(w + 49, k + 1)) {
        Log("Engine: the game's grab distance could not be redirected - the dominant grip's grab keeps the game's rule");
        return;
    }
    g_offGrabBody = U32(w + 11);
    g_offGrabWant = U32(g + 32);
    g_offGrabDist = U32(g + 4);
    g_grabMax = value(w + 8, w + 4);
    g_grabConst = k;
    g_grabOk = true;
    Log("Engine: game grab distance (grab +%llx, player update +%llx): body %x, wanted %x, now %x, max %.1f m; "
        "grabKeepDistance %d - held where taken, %.2f m from the eyes at least, dominant stick up / down",
        (unsigned long long)(g - g_base), (unsigned long long)(w - g_base), g_offGrabBody, g_offGrabWant, g_offGrabDist,
        g_grabMax, Cfg().grabKeepDistance ? 1 : 0, std::clamp(Cfg().grabMinDistance, 0.2f, 1.5f));
}

} // namespace

void EngineSetHandAim(bool onFoot)
{
    g_handAim = onFoot;
    g_handAimFrame = g_frameCounter;
}

bool EngineInit()
{
    g_base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (strncmp((const char*)sec->Name, ".text", 5) == 0) {
            g_text = g_base + sec->VirtualAddress;
            g_textSize = sec->Misc.VirtualSize;
        }
    if (!g_text) return false;

    // Game::frame: mov rcx,r12 ; call Game::update ; mov rcx,r12 ; call ... ; cmp byte [r12+8A0h],0
    BYTE* upd = Scan("49 8B CC E8 ?? ?? ?? ?? 49 8B CC E8 ?? ?? ?? ?? 41 80 BC 24 ?? ?? 00 00 00");
    // Game::frame, end of the time bookkeeping:
    //   movss [r12+214h],xmm0 ; ... addss xmm0,[r12+218h] ; ... movss [r12+218h],xmm0 ; movss [r12+204h],xmm6
    BYTE* tm = Scan("F3 41 0F 11 84 24 ?? ?? 00 00 F3 0F 59 D3 0F 28 C6 F3 41 0F 58 84 24 ?? ?? 00 00 F3 0F 5F C7 "
                    "F3 0F 5D C2 F3 41 0F 11 84 24 ?? ?? 00 00 F3 41 0F 11 B4 24 ?? ?? 00 00");
    InitToolHook();
    if (!upd || !tm) {
        Log("Engine: not recognised (update=%p time=%p) - synchronised stereo unavailable", upd, tm);
        return false;
    }
    void* updateFn = RelTarget(upd + 8, upd + 4);
    g_offAcc1 = U32(tm + 6);
    g_offAcc2 = U32(tm + 41);
    g_offDt = U32(tm + 51);
    Log("Engine: update=+%llx dt=%x accumulators=%x/%x", (unsigned long long)((BYTE*)updateFn - g_base), g_offDt,
        g_offAcc1, g_offAcc2);
    if (!HookFunction(updateFn, (void*)Hook_Update, (void**)&Real_Update)) return false;
    InitCameraHook();
    InitScriptCamera();
    InitMapView();
    InitScriptView();
    InitUiCamera();
    InitCrosshairAim();   // (after InitUiCamera)
    InitModUiPointer();   // (after InitCrosshairAim: its UI hooks)
    InitPlayerYaw();
    InitAimCamera();   // (after InitPlayerYaw and InitScriptCamera: needs the local player and the mod camera mode)
    InitModPanel();    // (after InitModUiPointer and InitAimCamera: the windows' hooks, the Lua argument count)
    InitHistoryHook();
    InitParticleHook();
    InitParticleSort();   // (after InitParticleHook: its update hook gives the systems)
    InitParticleMotion(); // (after InitParticleHook and InitHistoryHook: the systems, the frame; the right eye's history)
    InitShapeJobSkip();   // (after InitHistoryHook: FixObjectMotion, Hook_Render, the shape wait)
    InitDeadPasses();     // (after InitHistoryHook: Hook_Render gives the renderer)
    InitSameNoise();      // (after InitHistoryHook: Renderer::render's address, Hook_Render)
    InitOffArm();
    InitDualWield();   // (after InitAimCamera and InitOffArm: GetCameraTransform's trampoline, the handle lookup)
    InitGrabDistance();   // (after InitPlayerYaw: the local player)
    InitPerfTrace();   // ([Debug] perfTrace only: nothing searched nor hooked otherwise)
    InitLazyBegin();   // ([Performance] lazyBeginFrame only: nothing searched nor hooked otherwise)
    InitPauseWide();   // (after InitUiCamera: the UI camera it changes)
    g_ready = true;
    Log("Engine: synchronised stereo ready");
    return true;
}

bool EngineStereoAvailable() { return g_ready; }

unsigned long long EngineCatchUpDrops() { return g_catchUpDrops; }

void EngineRequestStereo()
{
    if (!g_ready) return;
    g_leftThisFrame = true;
    g_rightPending = true;
}

bool EngineInFrozenFrame()
{
    ++g_requestsThisUpdate;
    if (!g_inUpdate && g_ready && Cfg().debugLua && g_anomalies < 60) {
        ++g_anomalies;
        Log("Engine diag: Lua request outside Game::update (after update #%llu)", (unsigned long long)g_updates);
    }
    return g_inFrozenUpdate;
}

int EngineConsumeFrameEye()
{
    int eye = g_rightThisFrame ? 1 : (g_leftThisFrame ? 0 : -1);
    g_rightThisFrame = false;
    g_leftThisFrame = false;
    return eye;
}

void EngineCancelStereo()
{
    g_rightPending = false;
}

bool EngineNextFrameFrozen() { return g_rightPending; }

void EngineSetVrActive(bool active) { g_vrActive = active; }

void EngineSetVrFrameDelta(float seconds) { g_vrFrameDelta = seconds; }

void EngineSetLeftEyeRotation(const Quat& q) { g_leftEyeRot = q; }

void EngineSetLeftEyePosition(const Vec3& p) { g_leftEyePos = p; }

void EngineSetVrFov(float engineFovDeg) { g_vrFov = engineFovDeg; }

void EngineSetEyeProjection(int eye, float tanLeft, float tanRight, float tanUp, float tanDown, bool enabled)
{
    if (eye < 0 || eye > 1) return;
    g_eyeTan[eye][0] = tanLeft;
    g_eyeTan[eye][1] = tanRight;
    g_eyeTan[eye][2] = tanUp;
    g_eyeTan[eye][3] = tanDown;
    g_offCenter = enabled;
}

bool EngineMapAvailable() { return g_mapReady && g_ready; }

void EngineSetMapInput(float panX, float panY, float zoom)
{
    auto dz = [](float v) { return std::abs(v) < 0.15f ? 0.0f : v; };
    g_mapPan[0] = dz(panX);
    g_mapPan[1] = dz(panY);
    g_mapZoom = dz(zoom);
}

void EngineSetGrabStick(float metresPerSecond) { g_grabStickVel = metresPerSecond; }

bool EngineGrabKeepAvailable() { return g_grabOk; }

bool EngineWideFrame() { return WideNow(); }

void EngineSetScreenLive(bool live) { g_wide.screenLive = live; }

bool EngineGameSize(int& w, int& h)
{
    if (!g_gameObj) return false;
    w = *(int*)g_gameObj;
    h = *(int*)(g_gameObj + 4);
    return w >= 320 && w <= 16384 && h >= 200 && h <= 16384;
}

bool EngineScriptCamera()
{
    if (!g_scriptCamMode || !g_gameObj || !g_offGamePlayer) return false;
    BYTE* camObj = *(BYTE**)(g_gameObj + g_offGamePlayer);
    return camObj && *(int*)camObj == g_scriptCamMode;
}

void EngineSetAnchorYaw(float degrees)
{
    g_anchorYaw = degrees;
    g_anchorFrame = g_frameCounter;
}

void EngineSetAimTarget(const Vec3& fromEye)
{
    g_aimRel = fromEye;
    g_aimRelFrame = g_frameCounter;
}

void EngineSetModAimDistance(float metres)
{
    const unsigned i = g_dolly.n++ % 8;
    g_dolly.dist[i] = std::isfinite(metres) ? std::clamp(metres, 0.0f, 2000.0f) : 0.0f;
    g_dolly.frame[i] = g_frameCounter;
}

void EngineSetEyeOffset(const Pose& rightFromLeft) { g_eyeOffset = rightFromLeft; }

void EngineSetPlayerYaw(float degrees)
{
    if (!g_offPlayerYaw || !g_gameObj) return;
    BYTE** list = *(BYTE***)(g_gameObj + g_offPlayerList);
    int index = *(int*)(g_gameObj + g_offPlayerIndex);
    if (!list || index < 0 || index > 64) return;
    if (BYTE* player = list[index]) *(float*)(player + g_offPlayerYaw) = degrees * 3.14159265f / 180.0f;
}

bool EnginePlayerYawAvailable() { return g_offPlayerYaw != 0; }

void EngineSetCameraNearPlayer(bool nearPlayer) { g_camNearPlayer = nearPlayer; }

EngineToolInfo EngineGetToolInfo() { return g_toolInfo; }

void EngineSetOffArm(int handBody, int forearmBody)
{
    g_offArm.bodies[0] = handBody;
    g_offArm.bodies[1] = forearmBody;
}

void EngineSetOffArmCamera(const Vec3& cameraFromHand)
{
    g_offArm.camRel = cameraFromHand;
    g_offArm.camFrame = g_frameCounter;
}

void EngineSetUiRays(const Pose& leftAim, bool leftOk, float leftTrigger, const Pose& rightAim, bool rightOk,
                     float rightTrigger)
{
    g_modUi.aim[0] = leftAim;
    g_modUi.aim[1] = rightAim;
    g_modUi.ray[0] = leftOk;
    g_modUi.ray[1] = rightOk;
    g_modUi.trigger[0] = leftTrigger;
    g_modUi.trigger[1] = rightTrigger;
    g_modUi.rayFrame = g_frameCounter;
}

bool EngineModUiShown() { return g_modUi.pair && g_frameCounter - g_modUi.pairFrame <= 2; }

int EngineModUiHand() { return EngineModUiShown() ? g_modUi.hand : -1; }

bool EngineModUiOnWindow(int hand)
{
    return EngineModUiShown() && (hand == 0 || hand == 1) && g_modUi.onWin[hand];
}

bool EngineModUiCursor(int eye, float& x, float& y)
{
    if (eye < 0 || eye > 1 || !g_modUi.pair || g_modUi.cursorFrame[eye] != g_frameCounter) return false;
    x = g_modUi.cursor[eye][0];
    y = g_modUi.cursor[eye][1];
    return true;
}

bool EngineGetModPanel(EngineModPanel& out)
{
    const ModPanelState& P = g_panel;
    if (!PanelSession() || g_frameCounter - P.shown > 2) return false;
    out.pose = P.moveNew ? P.moveTo : P.pose;
    out.k = P.k;
    out.au = P.au;
    out.av = P.av;
    out.cw = P.cw;
    out.ch = P.ch;
    out.box = P.hitOk;
    out.x0 = P.hit[0];
    out.y0 = P.hit[1];
    out.x1 = P.hit[2];
    out.y1 = P.hit[3];
    out.pointer = EngineModUiHand();
    // (only once this window's own input queries were read, none asking for the right button: a reader
    // that never decodes, or a window that never queries, keeps the safe rule - frame band, other hand)
    out.rmbFree = P.rmbHooks && P.argSeen && !P.rmbAware && P.age >= 0.25f;
    out.held = P.held;
    out.session = P.start;
    out.movable = P.kind == kPanelMenu;   // (the tool ring stays where PanelToolPlace put it)
    return true;
}

void EngineModPanelHold(int hand)
{
    ModPanelState& P = g_panel;
    P.held = hand == 0 || hand == 1 ? hand : -1;
    P.gliding = false;
    P.awayT = 0;
    if (P.held >= 0 && PanelLog())
        Log("Engine: mod panel grabbed by the %s hand (window %s)", hand ? "right" : "left",
            P.rmbAware                  ? "reads the right click"
            : P.rmbHooks && P.argSeen   ? "never read the right click"
            : P.rmbHooks                ? "right click not known yet: its input queries were not read"
                                        : "right click not watched");
}

void EngineModPanelMove(const Pose& pose)
{
    if (!std::isfinite(pose.p.x) || !std::isfinite(pose.p.y) || !std::isfinite(pose.p.z)) return;
    if (g_panel.kind == kPanelTool) return;   // (the revolver's ring: never moved by a grip)
    g_panel.moveTo = { Normalize(pose.q), pose.p };
    g_panel.moveNew = true;
}

void EngineModPanelRelease()
{
    ModPanelState& P = g_panel;
    if (P.held < 0) return;
    P.held = -1;
    if (P.kind == kPanelTool) return;   // (the revolver's ring: no place kept, it stays where PanelToolPlace put it)
    const Pose pose = P.moveNew ? P.moveTo : P.pose;
    const Vec3 eye = PanelEyeMid();
    const Vec3 d = pose.p - eye;
    const float dist = Length(d);
    if (!(dist > 0.05f)) return;
    // Its place relative to the gaze heading of the session (the window opens there again, in front of
    // the gaze it is opened with), its height and distance from the eyes as they are now.
    float yaw = std::atan2(-d.x, -d.z) - P.heading;
    while (yaw > kPi) yaw -= 2 * kPi;
    while (yaw < -kPi) yaw += 2 * kPi;
    P.place = { yaw / kDeg, std::asin(std::clamp(d.y / dist, -1.0f, 1.0f)) / kDeg, std::clamp(dist, 0.35f, 3.0f) };
    P.placed = true;
    P.cachePlaced = false;   // (the player's own place now, not one applied from an earlier opening)
    P.placeEye = eye;
    P.awayT = 0;
    const bool key = P.keyValid && P.anchorLocked;
    if (key) PanelStorePlace(P.key, P.place);
    else P.savePending = true;   // (its key once its anchor is locked: PanelLockAnchor)
    if (PanelLog())
        Log("Engine: mod panel placed (yaw %+.0f deg from the gaze at its opening, pitch %+.0f deg, %.2f m; anchor %.0f %.0f; "
            "k %.5f; %s)%s", P.place.yaw, P.place.pitch, P.place.dist, P.au, P.av, P.k, PanelPoseText(pose),
            key ? " - saved for this window" : " - saved once the window is recognised");
}

bool EngineModPanelCorners(int eye, float xy[8])
{
    const ModPanelState& P = g_panel;
    if (!PanelMenuSession() || eye < 0 || eye > 1 || P.qFrame[eye] != g_frameCounter) return false;
    const float m = EngineModPanel::kEdge;
    const float x0 = P.hitOk ? P.hit[0] - m : 0, y0 = P.hitOk ? P.hit[1] - m : 0;
    const float x1 = P.hitOk ? P.hit[2] + m : P.cw, y1 = P.hitOk ? P.hit[3] + m : P.ch;
    for (int k = 0; k < 4; ++k)
        if (!PanelProject(eye, (k & 1) ? x1 : x0, (k & 2) ? y1 : y0, xy[2 * k], xy[2 * k + 1])) return false;
    return true;
}

// --- Dual wield (see DualEye) ---------------------------------------------------------------------
void EngineSetDualGun(int gun, int what, const Vec3& v)
{
    if ((gun != 0 && gun != 1) || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return;
    DualGun& g = g_dual.gun[gun];
    if (what == 0) {
        g.m = v;
        g.mFrame = g_frameCounter;
        return;
    }
    // (the target as a world point now: the muzzle where the Lua mod measured it - the body is read
    // from the same memory as its GetBodyTransform, in the same call - plus the offset sent)
    BYTE* b = DualBody(gun);
    if (!b || !g.mFrame) return;
    g.target = Mul(DualBodyPose(b), Pose{ {}, g.m }).p + v;
    g.tFrame = g_frameCounter;
}

void EngineSetDualCopy(int body)
{
    if (body == g_dual.copy) return;
    g_dual.copy = body;
    g_dual.gun[0] = {};
    g_dualCopyFix.bodies[0] = body;
}

void EngineSetDualCopyCamera(const Vec3& cameraFromCopy)
{
    g_dualCopyFix.camRel = cameraFromCopy;
    g_dualCopyFix.camFrame = g_frameCounter;
}

// (present thread: the state published by the main thread at its last Lua request, at most 4 updates old)
bool EngineDualReady()
{
    return (g_dualReadyBits.load(std::memory_order_acquire) & 1) && g_frameCounter - g_dualReadyAt.load() <= 4;
}

bool EngineDualGunReady(int gun)
{
    if (gun != 0 && gun != 1) return false;
    return (g_dualReadyBits.load(std::memory_order_acquire) & (gun ? 1 : 3)) == (gun ? 1 : 3) &&
           g_frameCounter - g_dualReadyAt.load() <= 4;
}

void EngineDualClickBegin(int gun)
{
    g_dualClickId.fetch_add(1, std::memory_order_acq_rel);
    g_dualClickGun.store(gun == 0 ? 0 : 1, std::memory_order_release);
}

void EngineDualClickDelivered()
{
    if (g_dualClickGun.load(std::memory_order_acquire) < 0) return;
    g_dual.downId = g_dualClickId.load(std::memory_order_acquire);
    g_dual.downFrame = g_frameCounter;
    g_dual.downNormal = g_normalUpdates;
}

void EngineDualClickEnd() { g_dualClickGun.store(-1, std::memory_order_release); }

// (main thread, at each Lua request: also publishes the state for input.cpp)
void EngineDualState(int& allowed, int& shots, int& gun, int& ready)
{
    allowed = DualAllowedNow() ? 1 : 0;
    shots = g_dual.shots;
    gun = g_dual.shotGun;
    ready = (allowed && DualFresh(1) ? 1 : 0) | (allowed && DualFresh(1) && DualFresh(0) ? 2 : 0);
    g_dualReadyBits.store(ready, std::memory_order_release);
    g_dualReadyAt.store(g_frameCounter);
}

unsigned long long EngineFrameCounter() { return g_frameCounter; }

bool EngineHoldingSledge()
{
    // (seen within the last 4 engine frames; g_frameCounter may be read before the update that stored
    // g_sledgeFrame: compared without an unsigned wrap)
    const uint64_t seen = g_sledgeFrame.load();
    return g_sledgeHeld.load() && g_frameCounter <= seen + 4;
}
