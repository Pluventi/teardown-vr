#pragma once
#include <string>

struct ImFontAtlas;

// VR inventory: the game's tools and weapons (mods included), drawn like the
// game's own tool bar (data/ui/hud.lua, drawTool): one column per group, the
// group number in a small white box at the bottom of each column, the tool
// names stacked above it, the tool in hand in light yellow on a dark box, the
// game's own fonts (data/ui/font). Below the boxes, the tool in hand and its
// ammo, as at the bottom of the PC screen.
//
// It floats in front of the player's body (vr.cpp: "Floating panels",
// UpdateInventoryPanel): at the belt, tilted up toward the eyes, like a window
// worn by the body - turning the head does not move it; walking in the room,
// crouching and (option inventoryFollow) really turning round carry it. Pointed
// at and gripped (the grip of that hand, the one that grabs objects), it follows
// the hand's ray, and stays where it is let go (inventoryYaw / Forward / Down).
// The dominant stick click (R3) shows / hides it - or brings it back in front
// when it is far from where the player looks (VrInventoryRecall); it stays until
// the next click (it is only hidden meanwhile in menus, vehicles, the map, the
// settings and the views of mods that move the camera). Changing tools with the
// stick does not show it (except with the "inventoryOnSwitch" setting, off by
// default).
// While the player looks at it: point at a name and pull the trigger to take
// the tool, at a group number to take the next tool of the group; hold the
// trigger on a list and move the hand up or down to scroll it (like a phone),
// release while moving to let it glide.
//
// Only the Lua mod can read the tool registry ("game.tool.*"): it sends the
// list over the clipboard channel in short messages (at most 30 characters,
// see main.lua), only when the list changes:
//   "TDVRIB <ver> <count>"                                start of a pass over list <ver> (1..99)
//   "TDVRIT <ver> <i> <group> <chunks> <selectable> <ammo>"   tool i (1-based, game order)
//   "TDVRIN <ver> <i> <k> <text>"                         chunk k (1..9) of tool i's name
//   "TDVRIA <ver> <i> <selectable> <ammo>"                ammo / availability changed
//   "TDVRIC <ver> <i> <locked>"                           tool in hand (0 = none), switching locked
// Text fields are plain ASCII: other bytes (UTF-8 accents...) and '~' arrive
// as "~XX" (hex), a lone "~" is an empty text.
// The DLL's response to each Lua request ends with
//   " INV <ack> <selSeq> <selVer> <selIdx> <open>"
// ack = list version completely received, selSeq / selVer / selIdx = the
// latest tool picked in the headset (sequence number, list version, index;
// Lua applies it once, on a normal frame), open = the panel is shown.
namespace Inventory {

// Panel size in the headset: metres per texture pixel (times the "panel size" setting).
constexpr float kMetersPerPixel = 0.00058f;

// The game's fonts (medium.ttf / bold.ttf from data/ui/font), added to the UI's
// font atlas by Ui::Init, after its default font. Missing files: the default font.
void AddFonts(ImFontAtlas* atlas);

// --- Lua side (game script thread) ---------------------------------------------
void HandleLuaMessage(const std::string& text);   // a "TDVRI..." message
std::string ResponseFields();                     // " INV ..." appended to the response
void SetGameFlags(int luaFlags);                  // LuaRequest::flags (vehicle, grabbing, map, camera)
// The game's camera of the last frame was at the player's eyes (kLuaFlagCamNearPlayer):
// a mod camera there (scope, camera mod) keeps the normal head view, so the panel
// stays usable; a mod camera away from the player (plane, drone views) hides it.
bool CameraNearPlayer();

// --- Controls (present thread) ---------------------------------------------------
// Dominant stick click (R3): shows the panel / hides it (a panel shown for a
// moment by ShowBrief stays shown instead).
void Toggle();
// Tool changed with the stick: only with the "inventoryOnSwitch" setting (off by
// default), shows the panel for a moment, display only (the trigger keeps firing).
void ShowBrief();
// Hides the panel (until the next Toggle).
void Close();
// Once per present. allowed = false hides the panel for now (menus, vehicle,
// map, settings, mod camera): it comes back when allowed again, unless R3 hid it.
void Update(float dt, bool allowed);
bool Visible();
// Shown by R3 (not the brief display-only glimpse): its trigger and grips work.
bool Interactive();
// The hand moving the panel with its grip (vr.cpp, UpdatePanels), -1 = none: a light frame
// around it; a trigger press of that hand in progress on it is dropped (no tool picked when the
// grip masks that trigger).
void SetHeld(int hand);
// Put it straight in front of the body again at the next present (R3 recall, VrInventoryRecall).
void RequestPlace();
// The trigger of the hand pointing at the panel (Input::PointerHand) belongs to it
// (pointing at it while looking at it, a press that began on it). CapturesTrigger:
// that hand is the dominant one, the game must not use the tool. CapturesOffTrigger:
// it is the other hand, its trigger must not interact (E).
bool CapturesTrigger();
bool CapturesOffTrigger();
// Ray of the hand pointing at the panel (Input::PointerHand). hit = the ray
// crosses the panel's plane at (x, y) panel
// pixels (also outside the panel: a drag goes on past its edge), onPanel =
// inside the panel, trigger = 0..1, focused = the player looks toward the panel
// (a ray that crosses it while the player looks elsewhere, aiming at the floor
// for example, does not take the trigger from the game).
void SetPointer(bool hit, bool onPanel, float x, float y, float trigger, bool focused);
// True once after the panel appeared: put it straight in front of the body.
bool ConsumePlaceRequest();

// --- Rendering (inside the settings UI's ImGui frame, present thread) ------------
// Draws the panel in the top-left PanelW() x PanelH() pixels of the UI texture.
void Build(float dt);
int PanelW();
int PanelH();

} // namespace Inventory
