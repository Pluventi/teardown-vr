#pragma once
#include <string>

// Communication channel with the Lua mod.
//
// Teardown's Lua API exposes SetClipboardText() / GetClipboardText(). We hook
// the clipboard functions in the game's import table: a text starting with
// "TDVR" written by Lua never reaches the real clipboard, it is handed to the
// VR code instead, and the following GetClipboardText() returns the VR state.
// Normal copy/paste keeps working.

struct LuaRequest
{
    int   seq = 0;
    int   flags = 0;        // see kLuaFlag*
    float hapticL = 0, hapticR = 0;
    float vignette = 0;     // 0..1, how much the player is moving with the stick
};

constexpr int kLuaFlagVehicle   = 1;
constexpr int kLuaFlagGrabbing  = 2;
constexpr int kLuaFlagMap       = 4;
constexpr int kLuaFlagHasTool   = 8;
constexpr int kLuaFlagCamNearPlayer = 16;   // the game's camera of the last frame was at the player's eyes (main.lua)
constexpr int kLuaFlagOffFarHold = 32;      // the off hand holds an object at a distance (main.lua, tdvrOffFarHold)

using LuaHandler = std::string (*)(const LuaRequest& req);

void BridgeInit(LuaHandler handler);
// Diagnostics: logs the last clipboard calls.
void BridgeDumpTrace();
