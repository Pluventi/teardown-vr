#pragma once
#include <windows.h>

// The PC keyboard keys, mouse buttons and wheel steps the VR mod can press for
// the player (keys panel, quick key, keys used by other mods). Each one is named
// as in Teardown's input API ("q", "f1", "space", "lmb"...); wheel steps are
// "wheelup" / "wheeldown". F8 and F10 are left out: they are Teardown VR's own
// hotkeys (recenter, settings panel).
namespace PcKeys {

enum Kind : unsigned char { Key, ExtKey, Mouse, Wheel };

struct Def
{
    const char* name;    // Teardown input name
    const char* label;   // text on the panel's button, in the language in use (Get picks it at each call)
    WORD vk;             // virtual key; Mouse: button 0..2; Wheel: 1 = up, 0 = down
    WORD scan;           // scan code of the key's position on a US keyboard
    Kind kind;
};

int        Count();
Def        Get(int key);               // (a copy: its label follows a language switch, i18n.h)
int        ByName(const char* name);   // case-insensitive, -1 if unknown
bool       IsModifier(int key);        // shift, ctrl, alt
bool       IsKeyboard(int key);        // a keyboard key (not a mouse button or a wheel step)

} // namespace PcKeys
