#pragma once
#include "vrmath.h"

// Hooks into the Teardown engine itself. Call once from the first Present.
// Returns false if the game version is not recognised.
bool EngineInit();

// Synchronised stereo: the frame after a left-eye frame is a "zero time"
// frame (the world does not move) rendered from the right eye, so both eyes
// show the same simulation state.
bool EngineStereoAvailable();
// Physics steps the "no catch-up" option dropped since start (diagnostics).
unsigned long long EngineCatchUpDrops();
// Called while the Lua mod renders the left eye: the next frame will be frozen.
void EngineRequestStereo();
// True while the frozen frame's update runs (the Lua mod must render the right eye).
bool EngineInFrozenFrame();
// At Present: which eye this frame rendered (0 left, 1 right, -1 none).
int  EngineConsumeFrameEye();
void EngineCancelStereo();
// True while the next game frame will be a frozen frame (input events wait,
// so that the game's "just pressed" logic sees them on a normal frame).
bool EngineNextFrameFrozen();
// The Lua mod drives the camera this frame (enables the VR-only engine tweaks,
// e.g. the tool held in the hand in first person).
void EngineSetVrActive(bool active);
// Pose of the right eye seen from the left eye, in game units (eye separation
// and world scale applied). The right-eye frame is rendered from exactly the
// left-eye frame's camera moved by this offset.
void EngineSetEyeOffset(const Pose& rightFromLeft);
// Turns the local player (left/right look angle, degrees) without moving it.
// The Lua API can only set yaw together with the position (which made the
// camera and the tool shake when called every frame).
void EngineSetPlayerYaw(float degrees);
bool EnginePlayerYawAvailable();
// Time between the headset's predicted display times of two stereo pairs. The
// next left-eye frame advances the game by exactly this (instead of the CPU
// time measured by the game), so motion is even on the headset's display.
void EngineSetVrFrameDelta(float seconds);
// View rotation of the left eye in the headset's (recentred) space, and the yaw
// (degrees) of the VR room in the game world, sent by the Lua mod. The rendered
// view rotation is built from these alone (the game's own camera rotation
// follows the player's aim and made the view shake when the arm moved).
void EngineSetLeftEyeRotation(const Quat& q);
void EngineSetAnchorYaw(float degrees);
// The controller's laser target relative to this frame's eye camera, sent by the Lua
// mod in left-eye frames while it aims with the controller on foot. Mods' 2D
// crosshairs drawn at the centre of the canvas are moved onto it (engine.cpp,
// AimUiOffset).
void EngineSetAimTarget(const Vec3& fromEye);
// Distance (metres) along the axis of a mod's camera to the point it aims at, sent by the
// Lua mod (TDVRZ) in left-eye frames while a mod's camera is away from the player; 0 =
// nothing hit. The mod's zoom moves the camera toward that point (engine.cpp, DollyTravel).
void EngineSetModAimDistance(float metres);
// Head (left eye) position in the headset's recentred space, metres, and the
// field of view sent to SetCameraFov - used by the 3D level map.
void EngineSetLeftEyePosition(const Vec3& p);
void EngineSetVrFov(float engineFovDeg);
// Off-centre projection per eye (0 left, 1 right): tangents of the left, right,
// up and down half-angles (all positive). enabled=false: the game's symmetric one.
void EngineSetEyeProjection(int eye, float tanLeft, float tanRight, float tanUp, float tanDown, bool enabled);
// Level map in 3D (a scale model seen from above) instead of the flat view.
bool EngineMapAvailable();
// The tool in the VR hand, as placed by the last tool update (for the Lua mod).
//   kind 0: no tool, or not placed by the DLL (old behaviour: simply in the hand);
//   kind 1: a built-in tool, held from its prefab (fp_action / tp_action_rh);
//   kind 2: a tool added by a mod. The DLL draws it at  override x flat, where
//           flat is the pose the mod gives it on a desktop screen (eye space,
//           SetToolTransform, its own animations included), shrunk by the tool's
//           scale toward the eye (Hook_ToolAnim). The Lua mod decides
//           which point of the tool the hand holds by moving the override.
// hasHand / hand: the right hand the mod placed on its tool this frame
// (SetToolHandPoseLocalTransform, tool space), when it does.
struct EngineToolInfo
{
    int kind = 0;
    Pose flat;
    bool hasHand = false;
    Vec3 hand;
    float scale = 1.0f;   // size given to a mod's tool (its shapes scaled about its muzzle)
    Vec3 pivot;           // that muzzle, in the tool body (unscaled); zero while not scaled
};
EngineToolInfo EngineGetToolInfo();
// Map controls this frame: pan (-1..1, right / forward) and zoom (+ = closer).
void EngineSetMapInput(float panX, float panY, float zoom);
// grabKeepDistance: how fast the dominant stick moves the object the game's grab holds (m/s, + = farther
// from the eyes, 0 = still; engine.cpp, GrabControls). Available: the grab's code was recognised (if not,
// the game's own rule: 1.5 m from the eyes at least, the mouse wheel's 25 cm steps).
void EngineSetGrabStick(float metresPerSecond);
bool EngineGrabKeepAvailable();
// A script (e.g. a mod's plane, drone or turret camera) sets the camera itself
// this frame: SetCameraTransform puts the camera in its "script" mode.
bool EngineScriptCamera();
// The size the game draws at (window pixels as the game sees them: Game+0 / +4, the
// size its UI camera is rebuilt with every frame). It reads mouse positions in that
// space. false if not known yet.
bool EngineGameSize(int& w, int& h);
// [Ecran] pauseWide: this frame - a paused level the headset shows on the virtual screen - was drawn
// for a 16:9 screen: its 2D interface laid out on a 1920 x 1080 canvas and its 3D view 16:9, both
// stretched over the whole window (engine.cpp, WideState). vr.cpp then shows the whole back buffer
// on a 16:9 quad. Asked at Present.
bool EngineWideFrame();
// vr.cpp, every Present: frames that are not eye images go to the headset's virtual screen (the
// session runs and shows images). A paused level is drawn 16:9 only then.
void EngineSetScreenLive(bool live);
// Mods aim with the controller: while the VR player is on foot (vr.cpp, every Lua
// request), other scripts' GetCameraTransform() returns the player's camera, which
// the Lua mod turns toward the controller's laser target (engine.cpp, AimCamera).
// Not during a mod's camera (SetCameraTransform) or the map; the view stays the head.
void EngineSetHandAim(bool onFoot);
// The Lua mod: the game's camera of the last frame was at the player's eyes. A
// mod's SetCameraTransform there is a first-person camera tweak: the normal head
// view is kept (otherwise the mod's camera is shown in VR, see engine.cpp).
void EngineSetCameraNearPlayer(bool nearPlayer);
// The hand that holds no tool (main.lua, tdvrOffArmTick): its two static bodies (hand,
// forearm; script handles), and, in each left-eye frame the Lua mod places them, the eye
// camera they were placed for minus the hand body's position (world). At render their
// shapes are moved onto the camera actually drawn (engine.cpp, OffArmLateFix).
void EngineSetOffArm(int handBody, int forearmBody);
void EngineSetOffArmCamera(const Vec3& cameraFromHand);
// Mods' interactive 2D windows (UiMakeInteractive: THE EVERYTHING REVOLVER's ammo menu...)
// used with either controller (engine.cpp, ModUiPointer). vr.cpp gives the aim rays of
// the left-eye frame's Lua request (recentred room space, like EngineSetLeftEyePosition);
// ok = false for a hand that is not tracked or points at one of our own panels.
void EngineSetUiRays(const Pose& leftAim, bool leftOk, float leftTrigger, const Pose& rightAim, bool rightOk,
                     float rightTrigger);
// Such a window is shown in VR (its canvas converges at modUiDistance, or it is a floating panel - see
// EngineGetModPanel; the mouse follows a hand).
bool EngineModUiShown();
// The hand pointing at it (0 left, 1 right), -1 when none / no window: its trigger and
// grip are the mouse buttons (input.cpp).
int  EngineModUiHand();
// Is that hand's ray (0 left, 1 right) on the window? (found by the left eye of the last pair)
bool EngineModUiOnWindow(int hand);
// Its cursor in this eye's image (0..1), for this frame; false when none.
bool EngineModUiCursor(int eye, float& x, float& y);
// A mod's menu shown as a floating panel (modUiPanel; engine.cpp, ModPanel): fixed in the room,
// drawn in each eye through the panel, moved with a grip (vr.cpp, GripTarget). Also the revolver's
// ammo ring (a window drawing on the held tool) with modUiToolFixed: fixed in the room straight ahead
// at eye height (PanelToolPlace), but never moved by a grip (movable = false).
struct EngineModPanel
{
    static constexpr float kEdge = 30.0f;    // canvas units around the content still counted as content
    static constexpr float kBand = 200.0f;   // ... then its frame band, up to here (a grip there moves it)
    Pose pose;              // the canvas point (au, av) on the panel: recentred headset space (like the
                            // Lua aim rays), +x right, +y up, +z toward the eyes
    float k = 0;            // metres per canvas unit
    float au = 0, av = 0;   // anchor (canvas units, y down)
    float cw = 0, ch = 0;   // canvas size
    bool box = false;       // its content box is known (else: the whole canvas)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // content box, canvas units
    int pointer = -1;       // the hand pointing at it (its trigger / grip are the mouse buttons)
    bool rmbFree = false;   // the window never read the right mouse button: a grip anywhere on it moves it
    int held = -1;          // the hand moving it
    unsigned long long session = 0;   // which window session (another one: a hold of the previous one ends)
    bool movable = true;    // a grip can take it (false: the revolver's ring - its grips stay the window's / the game's)
};
// False when no mod window is shown on a panel (none, the options off, the revolver's ring head-locked...).
bool EngineGetModPanel(EngineModPanel& out);
// A hand takes it (0 left, 1 right; -1 none): it is no longer brought back in front meanwhile.
void EngineModPanelHold(int hand);
// While held: its new pose (recentred headset space), used from the next stereo pair (both eyes alike).
void EngineModPanelMove(const Pose& pose);
// Let go: its place is kept for that window (relative to the gaze it opens with; modUiPanelPlaces,
// saved off this thread) and it stays where it is.
void EngineModPanelRelease();

// --- Dual wield (engine.cpp, DualEye): a mod tool made of two guns fired alternately by one mouse
// button (Total Overdose) is split by the Lua mod (main.lua, tdvrDualTick): the dominant hand keeps
// the gun on its side (the game's tool, the other gun erased), the other hand holds a copy of it.
// Each trigger press is one click (input.cpp); the shot of that click leaves the gun of the hand
// that pressed, toward its laser target. Guns: 1 = dominant hand (the tool), 0 = the other hand (copy).
// "Allowed": the held tool is a dual-wield tool (profile), the setting is on, the eye hook is
// installed, VR on foot aiming with the controllers - the Lua mod then splits the tool and sends its
// guns (" DUA ...", EngineDualState).
// From the Lua mod: what = 0, the muzzle of gun `gun` in its body (the tool / the copy; drawn size);
// what = 1, its laser target minus that muzzle (world axes) - kept as a world point at once.
void EngineSetDualGun(int gun, int what, const Vec3& v);
// The off hand's gun copy (script handle of its static body; 0 = none), and the eye camera it was
// placed for minus its position, in a frame the Lua mod placed it (late fix at render, like the arm).
void EngineSetDualCopy(int body);
void EngineSetDualCopyCamera(const Vec3& cameraFromCopy);
// Allowed, and gun 1's muzzle and target fresh (input.cpp then maps the triggers to the guns). Safe on
// the present thread: the state the main thread published at its last Lua request (EngineDualState).
bool EngineDualReady();
// Gun 0 (the off hand) too: its copy shown, muzzle and target fresh (else its trigger does nothing).
bool EngineDualGunReady(int gun);
// Input (present thread): our left click for gun `gun` is posted / its down reached the game's window
// (window procedure, main thread) / the click is over (its up was handled, one game update after).
void EngineDualClickBegin(int gun);
void EngineDualClickDelivered();
void EngineDualClickEnd();
// For the Lua mod (" DUA <allowed> <shots> <gun of the last shot> <ready>"): shots given to a gun so far;
// ready: + 1 ready, + 2 the off hand's gun too. Main thread, at each Lua request (also publishes it
// for EngineDualReady).
void EngineDualState(int& allowed, int& shots, int& gun, int& ready);
// Game updates so far (Game::update calls).
unsigned long long EngineFrameCounter();
// The VR hand holds the game's sledge (its tool update saw it in the last few engine frames).
bool EngineHoldingSledge();
// The corners of its content box (+ kEdge) in this eye's image (0..1), this frame: the marks of the
// zone a grip moves it from (vr.cpp).
bool EngineModPanelCorners(int eye, float xy[8]);
