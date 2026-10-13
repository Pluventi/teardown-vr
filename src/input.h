#pragma once
#include <windows.h>
#include "vr.h"

// Desktop window integration (hotkeys, settings UI mouse) and translation of
// the VR controllers into the game's own keyboard / mouse actions.
namespace Input {

enum class Mode { Off, Gameplay, Screen, Map };

void InstallHooks();     // IAT hooks, call from DllMain
void Init(HWND hwnd);
HWND GameWindow();
// A hammer swing gesture was detected: press "use tool" briefly.
void RequestSwing();
// The player is in a vehicle (the left stick then drives it).
void SetVehicle(bool inVehicle);
// The game's grab (dominant hand's grip) holds an object (Lua flag kLuaFlagGrabbing): the left mouse
// button would throw it, so the off hand's trigger does not fire a dual-wield gun then.
void SetPlayerGrabbing(bool grabbing);
// The off hand holds an object at a distance (Lua flag kLuaFlagOffFarHold, main.lua grabFar).
void SetOffFarHold(bool holding);
// grabKeepDistance: how fast the dominant stick moves the object the off hand holds at a distance (m/s,
// + = farther; 0 = still, or the stick is the other hand's): the " ARM" field (vr.cpp, main.lua).
float OffGrabStick();
// Called once per game frame.
void Update(Mode mode, const HandState& left, const HandState& right, bool uiOpen, float dt);
// Screen-mode laser pointer position on the game image (0..1), or <0 if none.
void SetScreenPointer(float u, float v);
// Re-posts input held back during frozen frames (call after each Present).
void FlushDeferred();

// --- Any PC key (keys panel, quick key, keys of other mods) -----------------------
// Keys are PcKeys indexes (pckeys.h). Tap = pressed briefly (long enough for the
// game to see it); hold = pressed while requested (the keys panel asks again every
// frame, -1 = none); latch = a modifier kept down while the keys panel is open.
void TapKey(int key);
void HoldKey(int key);
void SetLatched(int key, bool down);
bool Latched(int key);

// --- A stick used as the mouse (mods that move their own camera or read the mouse)
// Automatic: while a script sets the camera (EngineScriptCamera) unless the
// settings say never; "always" (keys panel) forces it until switched off.
void SetMouseAlways(bool on);
bool MouseAlways();
// The stick that acts as the mouse this frame: 0 = none, 1 = the movement stick,
// 2 = the rotation stick.
int  MouseStick();
// Zeroes that stick in the controller state sent to the Lua mod, so it does not
// also walk or turn.
void MaskMouseStick(HandState& left, HandState& right);

// --- Pointer hand: the controller that drives 2D interfaces ---------------------
// The settings panel, the VR inventory, the virtual screen (menus, pause) and, with
// the offHandPointer setting, mods' interactive windows (engine.cpp, ModUiPointer)
// follow the same hand: 0 = left, 1 = right. The dominant hand by default; with
// offHandPointer the other hand takes over ONLY when it pulls its trigger with its
// ray on one of them (that press is already its click, at its own ray). Otherwise the
// pointer stays with the hand that clicked last, also while only the other hand's ray
// is on an interface or crosses where the pointer is (as the player asked: the hands
// no longer swap the pointer by themselves). It never changes while the pointer
// hand's trigger is held, and never to a hand whose trigger is already held (an
// interface must see that press begin).
int  PointerHand();
// That hand pulled its trigger on a mod's interactive window (engine.cpp, ModUiPointer): with
// offHandPointer it is the pointer hand from now on. False: refused - the pointer hand's trigger
// is held (a press in progress keeps its hand, as above). Without offHandPointer: true, nothing
// changes here (the window keeps its own hand).
bool TakePointer(int hand);
// A hand's ray is on a 2D interface this frame, for an interface vr.cpp does not test
// itself (none yet: mods' windows have their own pointer, engine.cpp). Collected until
// the next UpdatePointerHand.
void ReportPointerHit(int hand);
// Once per present (vr.cpp), before the interfaces read PointerHand(): whether each
// hand's ray is on one of our panels, and both triggers (0..1).
void UpdatePointerHand(bool hitLeft, bool hitRight, float triggerLeft, float triggerRight);

// --- The off hand's grip on foot: crouch, or grab with the physical off hand ----------
// (main.lua, tdvrOffHandTick.) The Lua mod reports whether an object that hand can take is
// within its reach or under its laser (tdvrOffLaserTick: pulled into the hand), or whether
// it holds or pulls one ("TDVRH 0/1", repeated while true; a value older than 1.5 s counts
// as "no"). underHand: of those, an object right under the hand (not only under its laser) - "TDVRH
// <reach> <under hand>"; an older Lua mod sends no second field (false).
void SetOffHandReach(bool reach, bool underHand = false);
// The off hand touches an object it can take (or holds one): "TDVRH 1" within the last 1.5 s
// (Lua repeats it only every 4 stereo pairs - over 0.3 s below 26 fps - and sends "TDVRH 0" as
// soon as it changes: the longer window keeps no stale "yes").
// (With its laser on one of our panels Lua has no laser target - main.lua, tdvrOffLaserTick -
// so it then means "an object under the hand": its grip grabs that object, not the panel.)
bool OffHandTouching();
// An object the physical off hand can take is right under it (its grip takes it): that grip never
// moves a mod's menu panel then (vr.cpp, GripTarget) - same 1.5 s rule.
bool OffHandNear();
// "TDVRU" (main.lua, neutralizeGameWalk): the game sees a movement key held while the
// player walks with the stick only - release every movement key (next Update).
void RequestReleaseMovementKeys();
// The current press of that grip, decided when it went down and kept until it is
// released: 0 = up (or not on foot), 1 = crouch, 2 = grab. Sent to the Lua mod (" ARM ...").
int  OffGripPress();

} // namespace Input
