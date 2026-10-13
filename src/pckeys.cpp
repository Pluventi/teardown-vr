#include "pckeys.h"
#include "i18n.h"

#include <cstring>

namespace PcKeys {
namespace {

// A row of the table: the key - its label is the French text - and its English
// text when it differs (nullptr: the same in both languages). Get picks one.
struct Row
{
    Def def;
    const char* en = nullptr;
};

// Letters and digits carry their US virtual key AND their US scan code: on an
// AZERTY keyboard the letter Q is not at the Q position, and the game may read
// either one (see Input::SendKeyPos).
const Row kKeys[] = {
    { "a", "A", 'A', 0x1E, Key }, { "b", "B", 'B', 0x30, Key }, { "c", "C", 'C', 0x2E, Key },
    { "d", "D", 'D', 0x20, Key }, { "e", "E", 'E', 0x12, Key }, { "f", "F", 'F', 0x21, Key },
    { "g", "G", 'G', 0x22, Key }, { "h", "H", 'H', 0x23, Key }, { "i", "I", 'I', 0x17, Key },
    { "j", "J", 'J', 0x24, Key }, { "k", "K", 'K', 0x25, Key }, { "l", "L", 'L', 0x26, Key },
    { "m", "M", 'M', 0x32, Key }, { "n", "N", 'N', 0x31, Key }, { "o", "O", 'O', 0x18, Key },
    { "p", "P", 'P', 0x19, Key }, { "q", "Q", 'Q', 0x10, Key }, { "r", "R", 'R', 0x13, Key },
    { "s", "S", 'S', 0x1F, Key }, { "t", "T", 'T', 0x14, Key }, { "u", "U", 'U', 0x16, Key },
    { "v", "V", 'V', 0x2F, Key }, { "w", "W", 'W', 0x11, Key }, { "x", "X", 'X', 0x2D, Key },
    { "y", "Y", 'Y', 0x15, Key }, { "z", "Z", 'Z', 0x2C, Key },
    { "1", "1", '1', 0x02, Key }, { "2", "2", '2', 0x03, Key }, { "3", "3", '3', 0x04, Key },
    { "4", "4", '4', 0x05, Key }, { "5", "5", '5', 0x06, Key }, { "6", "6", '6', 0x07, Key },
    { "7", "7", '7', 0x08, Key }, { "8", "8", '8', 0x09, Key }, { "9", "9", '9', 0x0A, Key },
    { "0", "0", '0', 0x0B, Key },
    { "f1", "F1", VK_F1, 0x3B, Key }, { "f2", "F2", VK_F2, 0x3C, Key }, { "f3", "F3", VK_F3, 0x3D, Key },
    { "f4", "F4", VK_F4, 0x3E, Key }, { "f5", "F5", VK_F5, 0x3F, Key }, { "f6", "F6", VK_F6, 0x40, Key },
    { "f7", "F7", VK_F7, 0x41, Key }, { "f9", "F9", VK_F9, 0x43, Key }, { "f11", "F11", VK_F11, 0x57, Key },
    { "f12", "F12", VK_F12, 0x58, Key },
    { "esc", "Échap", VK_ESCAPE, 0x01, Key, "Esc" },
    { "tab", "Tab", VK_TAB, 0x0F, Key },
    { "space", "Espace", VK_SPACE, 0x39, Key, "Space" },
    { "return", "Entrée", VK_RETURN, 0x1C, Key, "Enter" },
    { "backspace", "Effacer", VK_BACK, 0x0E, Key, "Backspace" },
    { "shift", "Maj", VK_SHIFT, 0x2A, Key, "Shift" },
    { "ctrl", "Ctrl", VK_CONTROL, 0x1D, Key },
    // (sent as WM_KEYDOWN: a lone injected WM_SYSKEYUP of Alt would open the
    // window's system menu in DefWindowProc and freeze the game's message loop)
    { "alt", "Alt", VK_MENU, 0x38, Key },
    { "uparrow", "\xE2\x86\x91", VK_UP, 0x48, ExtKey },
    { "downarrow", "\xE2\x86\x93", VK_DOWN, 0x50, ExtKey },
    { "leftarrow", "\xE2\x86\x90", VK_LEFT, 0x4B, ExtKey },
    { "rightarrow", "\xE2\x86\x92", VK_RIGHT, 0x4D, ExtKey },
    { "insert", "Inser", VK_INSERT, 0x52, ExtKey, "Insert" },
    { "delete", "Suppr", VK_DELETE, 0x53, ExtKey, "Delete" },
    { "home", "Début", VK_HOME, 0x47, ExtKey, "Home" },
    { "end", "Fin", VK_END, 0x4F, ExtKey, "End" },
    { "pgup", "Page \xE2\x86\x91", VK_PRIOR, 0x49, ExtKey },
    { "pgdown", "Page \xE2\x86\x93", VK_NEXT, 0x51, ExtKey },
    { "lmb", "Clic gauche", 0, 0, Mouse, "Left click" },
    { "rmb", "Clic droit", 1, 0, Mouse, "Right click" },
    { "mmb", "Clic molette", 2, 0, Mouse, "Middle click" },
    { "wheelup", "Molette \xE2\x86\x91", 1, 0, Wheel, "Wheel \xE2\x86\x91" },
    { "wheeldown", "Molette \xE2\x86\x93", 0, 0, Wheel, "Wheel \xE2\x86\x93" },
};
constexpr int kCount = sizeof(kKeys) / sizeof(kKeys[0]);

// Other spellings found in mods.
const struct { const char* alias; const char* name; } kAliases[] = {
    { "escape", "esc" }, { "enter", "return" }, { "control", "ctrl" }, { "lshift", "shift" },
    { "lctrl", "ctrl" }, { "lalt", "alt" }, { "up_arrow", "uparrow" }, { "down_arrow", "downarrow" },
    { "left_arrow", "leftarrow" }, { "right_arrow", "rightarrow" }, { "pageup", "pgup" }, { "pagedown", "pgdown" },
};

} // namespace

int Count() { return kCount; }

Def Get(int key)
{
    const Row& r = kKeys[key < 0 || key >= kCount ? 0 : key];
    Def d = r.def;
    if (r.en) d.label = Tr(r.en, r.def.label);
    return d;
}

int ByName(const char* name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < kCount; ++i)
        if (_stricmp(kKeys[i].def.name, name) == 0) return i;
    for (const auto& a : kAliases)
        if (_stricmp(a.alias, name) == 0) return ByName(a.name);
    return -1;
}

bool IsModifier(int key)
{
    if (key < 0 || key >= kCount) return false;
    WORD vk = kKeys[key].def.vk;
    return kKeys[key].def.kind == Key && (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU);
}

bool IsKeyboard(int key)
{
    return key >= 0 && key < kCount && (kKeys[key].def.kind == Key || kKeys[key].def.kind == ExtKey);
}

} // namespace PcKeys
