#pragma once
#include <string>
#include <vector>

// Keys used by the mods running in the game, found without any help from the
// mods themselves:
//  - which mods run: the game logs "Active mod: <name> (<id>)" in
//    %LOCALAPPDATA%\Teardown\log.txt each time a level starts (before the first
//    level: the enabled mods of the current mod list, modlists\<id>.xml);
//  - their keys: their Lua scripts are read (InputPressed("q"), key tables such
//    as AC130_KEY_BINDS = { previousShell = "q" }, GLOBAL.noClipKey = "n"...),
//    plus the keys they saved in savegame.xml (key settings changed in their
//    options menu). Teardown actions ("interact", "usetool"...) are turned into
//    the player's key with options.xml.
// The scan runs on a background thread (a few MB of Lua, well under a second).
namespace ModKeys {

struct Entry
{
    std::string label;   // the mod's own name for it ("previousShell"), may be empty
    std::string input;   // as written by the mod ("q", "interact"...)
    std::string key;     // PcKeys name of the key to press ("q", "e", "lmb", "wheelup"...)
};

struct Mod
{
    std::string id, name;
    bool usesMouse = false;     // reads camerax / cameray / mousedx / mousedy
    std::vector<Entry> keys;
};

// Starts a new scan (ignored while one is running).
void Refresh();
bool Scanning();
// The last result; *serial changes with each new result.
std::vector<Mod> Get(unsigned* serial = nullptr);
unsigned Serial();

// Scan now on the calling thread (tests). Folders are found from the game
// folder (with trailing backslash) and %LOCALAPPDATA%\Teardown\.
std::vector<Mod> ScanNow(const std::wstring& gameDir, const std::wstring& appData, const std::wstring& documents);

} // namespace ModKeys
