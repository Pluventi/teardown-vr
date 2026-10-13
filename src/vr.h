#pragma once
#include "vrmath.h"
#include <string>
#include <vector>
#include <cstdint>

// Controller state shared with the input / UI modules (recentered tracking space).
struct HandState
{
    bool  active = false;
    Pose  aim, grip;
    float stickX = 0, stickY = 0;
    float trigger = 0, squeeze = 0;
    bool  stickClick = false;
    bool  primary = false;   // A / X
    bool  secondary = false; // B / Y
    bool  menu = false;      // left hand only
};

// A text the settings panel shows, kept in both languages: c_str() gives the one in use (i18n.h), picked each
// time it is read, so a language switch shows at once.
struct VrText
{
    std::string en, fr;
    const char* c_str() const;
    bool empty() const { return en.empty() && fr.empty(); }
};

struct VrStatus
{
    VrText state = { "Initializing...", "Initialisation..." };
    std::string runtime;
    std::string system;
    VrText runtimeDetect;             // [Affichage] headsetRuntime 0: the runtime the detection chose, and why
    bool  sessionRunning = false;
    bool  focused = false;
    bool  stereoActive = false;       // Lua mod is driving the camera
    bool  nativeStereo = false;       // both eyes rendered from the same frame
    int   recommendedW = 0, recommendedH = 0;
    float refreshRate = 0;
    std::vector<float> refreshRates;
    float gameFps = 0;
    float vrFps = 0;
    int   backbufferW = 0, backbufferH = 0;
    int   eyeImageW = 0, eyeImageH = 0;   // each eye image sent to the headset (swapchain; eyeResScale)
    int   backbufferFormat = 0;
    float renderFovH = 0, renderFovV = 0;
    float cropBase = 1;               // fovCrop: share of the field (each way) the back buffer was sized for at start
    int   luaRequests = 0;
};

const VrStatus& VrGetStatus();
// The headset session runs but the game was started before the headset was connected: its
// window and render targets have the monitor's size (the VR size is only chosen at start).
bool  VrRenderSizeLate();
void  VrRequestRecenter();
bool  VrGetHands(HandState& left, HandState& right);   // false if no session
bool  VrGetHead(Pose& head);
void  VrHaptic(int hand, float amplitude, float seconds);

// UI panel placement in VR (quad layer).
void  VrPlaceUiPanel();
bool  VrUiPanelVisible();

// Our floating panels (VR inventory, settings / keys panel) are moved with a grip (vr.cpp,
// UpdatePanelGrips): pressed while that hand's ray is on one, the press belongs to the panel -
// never the game's grab, the off hand's grab or crouch - until it is released. Hands: 0 left,
// 1 right.
bool  VrPanelOwnsGrip(int hand);
// That hand moves a panel (or did, and its trigger is still held): its trigger does nothing
// (a whole-fist squeeze must not fire, crouch nor click), until let go.
bool  VrPanelMasksTrigger(int hand);
int   VrPanelHoldHand();   // the hand moving a panel now (-1 = none)
// R3 with the VR inventory shown: true when it is far from where the player looks (more than
// 65 degrees: behind after turning round) and being put back at its place around the body's
// heading now (straight ahead, or the side it was put at: inventoryYaw is kept) brings it
// clearly closer - R3 then brings it back (Inventory::RequestPlace) instead of hiding it.
bool  VrInventoryRecall();

// Entry for the Lua bridge.
std::string VrHandleLuaRequest(const struct LuaRequest& req);
