#include "modkeys.h"
#include "config.h"
#include "log.h"
#include "pckeys.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace ModKeys {
namespace {

std::mutex g_mutex;
std::vector<Mod> g_result;
unsigned g_serial = 0;
std::atomic<bool> g_running{ false };

constexpr size_t kMaxLuaFiles = 800;
constexpr size_t kMaxLuaBytes = 4u << 20;   // per file
constexpr size_t kMaxKeysPerMod = 40;
constexpr size_t npos = std::string::npos;

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool Contains(const std::string& s, const char* part) { return s.find(part) != npos; }

std::string NoUnderscore(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    return s;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) s = p;
    CoTaskMemFree(p);
    return s;
}

// Reads a file the game may be writing at the same time (log.txt). Only the
// last maxBytes are kept.
bool ReadAll(const std::wstring& path, std::string& out, size_t maxBytes)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return false;
    }
    LARGE_INTEGER start;
    start.QuadPart = size.QuadPart > (LONGLONG)maxBytes ? size.QuadPart - (LONGLONG)maxBytes : 0;
    SetFilePointerEx(h, start, nullptr, FILE_BEGIN);
    out.resize((size_t)(size.QuadPart - start.QuadPart));
    size_t total = 0;
    DWORD got = 0;
    while (total < out.size() &&
           ::ReadFile(h, &out[total], (DWORD)std::min<size_t>(out.size() - total, 1u << 20), &got, nullptr) && got)
        total += got;
    out.resize(total);
    CloseHandle(h);
    return true;
}

// --- Teardown's registry files (options.xml, savegame.xml) ---------------------
using Path = std::vector<std::string>;

// Calls fn(path, value) for each element with a value="..." attribute; path =
// element names from the root (<registry>).
void WalkRegistry(const std::string& xml, const std::function<void(const Path&, const std::string&)>& fn)
{
    Path path;
    size_t i = 0, n = xml.size();
    while ((i = xml.find('<', i)) != npos && i + 1 < n) {
        size_t end = i + 1;   // the tag's '>' (outside quotes)
        bool quote = false;
        while (end < n && (quote || xml[end] != '>')) {
            if (xml[end] == '"') quote = !quote;
            ++end;
        }
        if (end >= n) break;
        char c = xml[i + 1];
        if (c == '?' || c == '!') {
            i = end + 1;
            continue;
        }
        if (c == '/') {
            if (!path.empty()) path.pop_back();
            i = end + 1;
            continue;
        }
        size_t s = i + 1, e = s;
        while (e < end && !isspace((unsigned char)xml[e]) && xml[e] != '/') ++e;
        path.push_back(xml.substr(s, e - s));
        size_t v = xml.find(" value=\"", e);
        if (v != npos && v < end) {
            size_t q = xml.find('"', v + 8);
            if (q != npos && q < end) fn(path, xml.substr(v + 8, q - v - 8));
        }
        if (xml[end - 1] == '/') path.pop_back();
        i = end + 1;
    }
}

// --- Teardown actions --------------------------------------------------------
// Inputs a mod may read instead of a key, with the game's default keys (the
// player's own are in options.xml, <keymap>). Names without '_'.
const struct { const char* action; const char* key; } kActions[] = {
    { "up", "w" }, { "down", "s" }, { "left", "a" }, { "right", "d" }, { "jump", "space" }, { "crouch", "ctrl" },
    { "usetool", "lmb" }, { "grab", "rmb" }, { "interact", "e" }, { "flashlight", "f" }, { "map", "tab" },
    { "pause", "esc" }, { "handbrake", "space" }, { "vehicleraise", "lmb" }, { "vehiclelower", "rmb" },
    { "vehicleaction", "space" }, { "scrollup", "wheel" }, { "scrolldown", "wheel" }, { "cameraview", "c" },
    { "scoreboard", "shift" },
};

using KeyMap = std::map<std::string, std::string>;

// The PcKeys name of the key a mod's input stands for ("" if none).
std::string ResolveKey(const std::string& input, const KeyMap& keymap)
{
    int k = PcKeys::ByName(input.c_str());
    if (k >= 0) return PcKeys::Get(k).name;
    std::string a = NoUnderscore(input);
    for (const auto& d : kActions) {
        if (a != d.action) continue;
        auto it = keymap.find(a);
        std::string key = it != keymap.end() && !it->second.empty() ? it->second : d.key;
        if (key == "wheel") key = a == "scrolldown" ? "wheeldown" : "wheelup";
        k = PcKeys::ByName(key.c_str());
        return k >= 0 ? PcKeys::Get(k).name : "";
    }
    return "";
}

// --- Lua scripts ----------------------------------------------------------------
struct Tok
{
    char type;          // 'i' name, 's' string, 'n' number, 'p' punctuation
    std::string text;
};

void Tokenize(const std::string& src, std::vector<Tok>& out)
{
    size_t i = 0, n = src.size();
    // "[[", "[==[" ...: returns the level (number of '='), or -1.
    auto longOpen = [&](size_t at) {
        size_t j = at + 1;
        while (j < n && src[j] == '=') ++j;
        return j < n && src[j] == '[' ? (int)(j - at - 1) : -1;
    };
    auto skipLong = [&](size_t from, int level) {
        std::string close = "]" + std::string(level, '=') + "]";
        size_t e = src.find(close, from);
        return e == npos ? n : e + close.size();
    };
    while (i < n) {
        char c = src[i];
        if (isspace((unsigned char)c)) {
            ++i;
        } else if (c == '-' && i + 1 < n && src[i + 1] == '-') {
            int level = i + 2 < n && src[i + 2] == '[' ? longOpen(i + 2) : -1;
            if (level >= 0) {
                i = skipLong(i + 4 + level, level);
            } else {
                while (i < n && src[i] != '\n') ++i;
            }
        } else if (isalpha((unsigned char)c) || c == '_') {
            size_t s = i;
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_')) ++i;
            out.push_back({ 'i', src.substr(s, i - s) });
        } else if (isdigit((unsigned char)c)) {
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '.')) ++i;
            out.push_back({ 'n', std::string() });
        } else if (c == '"' || c == '\'') {
            std::string s;
            ++i;
            while (i < n && src[i] != c && src[i] != '\n') {
                if (src[i] == '\\' && i + 1 < n) {
                    s += src[i + 1];
                    i += 2;
                    continue;
                }
                s += src[i++];
            }
            ++i;
            out.push_back({ 's', s.size() <= 32 ? s : std::string() });   // (a long text is never a key name)
        } else if (c == '[' && longOpen(i) >= 0) {
            int level = longOpen(i);
            i = skipLong(i + 2 + level, level);
            out.push_back({ 's', std::string() });
        } else if ((c == '=' || c == '~' || c == '<' || c == '>') && i + 1 < n && src[i + 1] == '=') {
            out.push_back({ 'p', src.substr(i, 2) });
            i += 2;
        } else if (c == '.' && i + 1 < n && src[i + 1] == '.') {
            out.push_back({ 'p', ".." });
            i += 2;
            if (i < n && src[i] == '.') ++i;
        } else {
            out.push_back({ 'p', std::string(1, c) });
            ++i;
        }
    }
}

// A name that says "key setting".
bool KeyWord(const std::string& lowerName)
{
    return Contains(lowerName, "key") || Contains(lowerName, "bind") || Contains(lowerName, "input") ||
           Contains(lowerName, "control") || Contains(lowerName, "button");
}

// Names too common to tell which variable holds a key.
bool Generic(const std::string& lowerName)
{
    static const char* kNames[] = { "self", "k", "v", "i", "j", "t", "s", "p", "c", "key", "keys", "name", "input",
                                    "inputs", "action", "actions", "control", "controls", "bind", "binds", "binding",
                                    "bindings", "config", "cfg", "options", "settings", "global", "data", "value",
                                    "id", "button", "buttons", "string", "tostring", "getstring", "playerid", "pcall" };
    for (const char* g : kNames)
        if (lowerName == g) return true;
    return false;
}

bool IsInputFn(const std::string& s)
{
    return s == "InputPressed" || s == "InputDown" || s == "InputReleased" || s == "InputValue";
}

// Pass 1: InputPressed("q") (also through pcall(InputPressed, "q")) and the
// variables read by InputPressed(variable) (AC130_KEY_BINDS, noClipKey...).
void FindInputCalls(const std::vector<Tok>& toks, std::vector<std::string>& direct, std::set<std::string>& argNames)
{
    for (size_t t = 0; t + 2 < toks.size(); ++t) {
        if (toks[t].type != 'i' || !IsInputFn(toks[t].text)) continue;
        bool call = toks[t + 1].type == 'p' && toks[t + 1].text == "(";
        bool viaPcall = toks[t + 1].type == 'p' && toks[t + 1].text == "," && t >= 2 && toks[t - 1].text == "(" &&
                        toks[t - 2].text == "pcall";
        if (!call && !viaPcall) continue;
        size_t a = t + 2;
        if (toks[a].type == 's') {
            if (a + 1 < toks.size() && toks[a + 1].type == 'p' && (toks[a + 1].text == ")" || toks[a + 1].text == ","))
                direct.push_back(toks[a].text);
            continue;
        }
        int depth = 0;
        for (size_t u = a; u < toks.size() && u < a + 40; ++u) {
            const Tok& k = toks[u];
            if (k.type == 'p') {
                if (k.text == "(" || k.text == "[" || k.text == "{") {
                    ++depth;
                } else if (k.text == ")" || k.text == "]" || k.text == "}") {
                    if (depth-- == 0) break;
                } else if (k.text == "," && depth == 0) {
                    break;
                }
            } else if (k.type == 'i' && !Generic(Lower(k.text))) {
                argNames.insert(k.text);
            }
        }
    }
}

// Pass 2: key settings written in the scripts: name = "q" where the name says
// "key" or is read by an Input call, and every value of such a table
// (AC130_KEY_BINDS = { previousShell = "q" }, key = { throw = { "string", "r" } }).
// Values without a name count only in small tables: a long list is the list of
// every key of a key-binding menu (keyboardKeys = { "esc", "f1", ... }).
void FindKeySettings(const std::vector<Tok>& toks, const std::set<std::string>& argNames,
                     const std::function<void(const std::string&, const std::string&)>& add)
{
    struct Table
    {
        bool keys;
        std::string field;                                    // the current field's name
        std::vector<std::pair<std::string, std::string>> list;   // values without a name
        int values = 0;
    };
    std::vector<Table> stack;
    auto assigned = [&](size_t t) {
        return t >= 2 && toks[t - 1].type == 'p' && toks[t - 1].text == "=" && toks[t - 2].type == 'i';
    };
    auto named = [&](const std::string& n) { return KeyWord(Lower(n)) || argNames.count(n) > 0; };
    for (size_t t = 0; t < toks.size(); ++t) {
        const Tok& k = toks[t];
        if (k.type == 'p' && k.text == "=") {
            if (t >= 1 && toks[t - 1].type == 'i' && !stack.empty()) stack.back().field = toks[t - 1].text;
            continue;
        }
        if (k.type == 'p' && k.text == "{") {
            bool parent = !stack.empty() && stack.back().keys;
            if (assigned(t)) stack.push_back({ parent || named(toks[t - 2].text), toks[t - 2].text });
            else stack.push_back({ parent, stack.empty() ? std::string() : stack.back().field });
            continue;
        }
        if (k.type == 'p' && k.text == "}") {
            if (stack.empty()) continue;
            if (stack.back().values <= 3)
                for (const auto& [field, value] : stack.back().list) add(field, value);
            stack.pop_back();
            continue;
        }
        if (k.type != 's') continue;
        bool inKeys = !stack.empty() && stack.back().keys;
        if (assigned(t)) {
            if (!k.text.empty() && (inKeys || named(toks[t - 2].text))) add(toks[t - 2].text, k.text);
        } else if (!stack.empty() && !(t >= 1 && toks[t - 1].text == "[")) {
            ++stack.back().values;
            if (inKeys && !k.text.empty()) stack.back().list.push_back({ stack.back().field, k.text });
        }
    }
}

void ListLua(const std::wstring& dir, int depth, std::vector<std::wstring>& out)
{
    if (depth > 8 || out.size() >= kMaxLuaFiles) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ListLua(dir + n + L"\\", depth + 1, out);
        } else if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".lua") == 0 && fd.nFileSizeHigh == 0 &&
                   fd.nFileSizeLow <= kMaxLuaBytes) {
            out.push_back(dir + n);
        }
    } while (out.size() < kMaxLuaFiles && FindNextFileW(h, &fd));
    FindClose(h);
}

// --- Active mods ------------------------------------------------------------------
struct Active { std::string id, name; };

bool SafeId(const std::string& id)
{
    if (id.empty() || id.size() > 120 || Contains(id, "..")) return false;
    for (char c : id)
        if (c == '\\' || c == '/' || c == ':' || (unsigned char)c < 32) return false;
    return true;
}

bool IsOurs(const std::string& id) { return Contains(Lower(id), "teardownvr"); }

std::vector<Active> ActiveMods(const std::wstring& appData, std::string& source)
{
    std::vector<Active> mods;
    std::string listName;
    std::string log;
    if (ReadAll(appData + L"log.txt", log, 8u << 20)) {
        std::vector<std::pair<size_t, size_t>> lines;
        for (size_t p = 0; p < log.size();) {
            size_t e = log.find('\n', p);
            if (e == npos) e = log.size();
            lines.push_back({ p, e });
            p = e + 1;
        }
        const std::string tag = "Active mod: ", listTag = "Loading mod list: ";
        // (searched within the line only: a search running on to the end of the log for
        // every line was quadratic - seconds to minutes on a log of several MB, e.g. the
        // AC130 mod logs every shell it spawns)
        auto find = [&](size_t line, const std::string& what) {
            const char* b = log.data() + lines[line].first;
            const char* e = log.data() + lines[line].second;
            const char* f = std::search(b, e, what.begin(), what.end());
            return f == e ? npos : (size_t)(f - log.data());
        };
        int last = -1;
        for (int i = (int)lines.size() - 1; i >= 0 && (last < 0 || listName.empty()); --i) {
            if (last < 0 && find(i, tag) != npos) last = i;
            size_t l = listName.empty() ? find(i, listTag) : npos;
            if (l != npos) {
                listName = log.substr(l + listTag.size(), lines[i].second - l - listTag.size());
                while (!listName.empty() && (listName.back() == '\r' || listName.back() == ' ')) listName.pop_back();
            }
        }
        // The last run of consecutive "Active mod: <name> (<id>)" lines = the level being played.
        int first = last;
        while (first > 0 && find(first - 1, tag) != npos) --first;
        for (int i = first; last >= 0 && i <= last; ++i) {
            size_t f = find(i, tag) + tag.size();
            std::string rest = log.substr(f, lines[i].second - f);
            while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
            size_t open = rest.rfind(" ("), close = rest.rfind(')');
            if (open != npos && close == rest.size() - 1 && close > open + 2)
                mods.push_back({ rest.substr(open + 2, close - open - 2), rest.substr(0, open) });
        }
        source = "log.txt";
    }
    bool others = std::any_of(mods.begin(), mods.end(), [](const Active& a) { return !IsOurs(a.id); });
    if (others) return mods;

    // No level started yet (or only Teardown VR in it): the current mod list.
    mods.clear();
    std::string index, list;
    if (!ReadAll(appData + L"modlists\\index.xml", index, 1u << 20)) return mods;
    std::string id;
    for (size_t p = 0; (p = index.find("<modlist id=\"", p)) != npos;) {
        p += 13;
        size_t q = index.find('"', p);
        if (q == npos) break;
        std::string thisId = index.substr(p, q - p);
        size_t a = index.find("<name>", q), b = a == npos ? npos : index.find("</name>", a);
        std::string name = b == npos ? std::string() : index.substr(a + 6, b - a - 6);
        if (id.empty() || name == listName) id = thisId;
        if (name == listName) break;
    }
    if (id.empty() || id.find_first_not_of("0123456789") != npos) return mods;
    if (!ReadAll(appData + L"modlists\\" + Widen(id) + L".xml", list, 4u << 20)) return mods;
    for (size_t p = 0; (p = list.find("<mod id=\"", p)) != npos;) {
        p += 9;
        size_t q = list.find('"', p);
        if (q == npos) break;
        mods.push_back({ list.substr(p, q - p), std::string() });
    }
    source = "modlists\\" + id + ".xml";
    return mods;
}

// Folder of a mod from its id (with trailing backslash), "" if unknown.
std::wstring ModFolder(const std::string& id, const std::wstring& gameDir, const std::wstring& documents)
{
    if (id.rfind("steam-", 0) == 0) {
        // <library>\steamapps\common\Teardown\ -> <library>\steamapps\workshop\content\1167630\<n>\ .
        std::string n = id.substr(6);
        if (n.empty() || n.find_first_not_of("0123456789") != npos || gameDir.size() < 4) return L"";
        size_t a = gameDir.find_last_of(L"\\/", gameDir.size() - 2);
        size_t b = a == npos || a == 0 ? npos : gameDir.find_last_of(L"\\/", a - 1);
        if (b == npos) return L"";
        return gameDir.substr(0, b + 1) + L"workshop\\content\\1167630\\" + Widen(n) + L"\\";
    }
    if (id.rfind("builtin-", 0) == 0) return gameDir + L"mods\\" + Widen(id.substr(8)) + L"\\";
    if (id.rfind("local-", 0) == 0 && !documents.empty())
        return documents + L"\\Teardown\\mods\\" + Widen(id.substr(6)) + L"\\";
    return L"";
}

// "name = ..." in the mod's info.txt.
std::string InfoName(const std::wstring& folder)
{
    std::string info;
    if (!ReadAll(folder + L"info.txt", info, 64u << 10)) return "";
    for (size_t p = 0; p < info.size();) {
        size_t e = info.find('\n', p);
        if (e == npos) e = info.size();
        std::string line = info.substr(p, e - p);
        p = e + 1;
        size_t eq = line.find('=');
        if (eq == npos || Lower(line.substr(0, eq)).find("name") != 0) continue;
        std::string v = line.substr(eq + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
        return v;
    }
    return "";
}

using Saved = std::vector<std::pair<Path, std::string>>;   // path below <mod><id>, value

Mod ScanMod(const Active& a, const std::wstring& folder, const KeyMap& keymap, const Saved* saved)
{
    Mod mod;
    mod.id = a.id;
    mod.name = a.name;
    std::vector<std::wstring> files;
    ListLua(folder, 0, files);
    std::vector<std::vector<Tok>> toks(files.size());
    std::vector<std::string> direct;
    std::set<std::string> argNames;
    for (size_t i = 0; i < files.size(); ++i) {
        std::string src;
        if (!ReadAll(files[i], src, kMaxLuaBytes)) continue;
        Tokenize(src, toks[i]);
        FindInputCalls(toks[i], direct, argNames);
    }

    std::set<std::string> keysSeen, labelsSeen;
    auto add = [&](const std::string& label, const std::string& literal) {
        std::string in = Lower(literal);
        if (in == "camerax" || in == "cameray" || in == "mousedx" || in == "mousedy") {
            mod.usesMouse = true;
            return;
        }
        if (mod.keys.size() >= kMaxKeysPerMod || (!label.empty() && labelsSeen.count(label))) return;
        if (in == "mousewheel") {
            for (const char* k : { "wheelup", "wheeldown" })
                if (keysSeen.insert(k).second) mod.keys.push_back({ label, in, k });
            if (!label.empty()) labelsSeen.insert(label);
            return;
        }
        std::string key = ResolveKey(in, keymap);
        if (key.empty()) return;
        // (a key setting saved by the mod wins over the script's default of the same name)
        if (!label.empty()) labelsSeen.insert(label);
        if (keysSeen.insert(key).second) mod.keys.push_back({ label, in, key });
    };

    // Keys the player set in the mod's own options (savegame.xml).
    if (saved) {
        for (const auto& [path, value] : *saved) {
            bool keyish = false;
            for (const std::string& part : path) keyish = keyish || KeyWord(Lower(part)) || argNames.count(part);
            // (digits: mostly on / off settings, "1" is rarely a key)
            if (keyish && !path.empty() && !value.empty() && value.find_first_not_of("0123456789.-") != npos)
                add(path.back(), value);
        }
    }
    for (const auto& t : toks) FindKeySettings(t, argNames, add);
    for (const std::string& d : direct) add("", d);
    return mod;
}

} // namespace

std::vector<Mod> ScanNow(const std::wstring& gameDir, const std::wstring& appData, const std::wstring& documents)
{
    ULONGLONG t0 = GetTickCount64();
    std::string source;
    std::vector<Active> active = ActiveMods(appData, source);

    KeyMap keymap;
    std::string text;
    if (ReadAll(appData + L"options.xml", text, 4u << 20))
        WalkRegistry(text, [&](const Path& p, const std::string& v) {
            if (p.size() >= 2 && p[p.size() - 2] == "keymap") keymap[NoUnderscore(Lower(p.back()))] = Lower(v);
        });
    std::map<std::string, Saved> saved;
    if (ReadAll(appData + L"savegame.xml", text, 32u << 20))
        WalkRegistry(text, [&](const Path& p, const std::string& v) {
            if (p.size() >= 5 && p[1] == "savegame" && p[2] == "mod") saved[p[3]].push_back({ Path(p.begin() + 4, p.end()), v });
        });

    std::vector<Mod> result;
    int keys = 0;
    for (const Active& a : active) {
        if (IsOurs(a.id) || !SafeId(a.id)) continue;
        std::wstring folder = ModFolder(a.id, gameDir, documents);
        if (folder.empty() || GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        auto it = saved.find(a.id);
        Mod m = ScanMod(a, folder, keymap, it == saved.end() ? nullptr : &it->second);
        if (m.name.empty()) m.name = InfoName(folder);
        if (m.name.empty()) m.name = a.id;
        if (m.keys.empty() && !m.usesMouse) continue;
        keys += (int)m.keys.size();
        result.push_back(std::move(m));
    }
    Log("ModKeys: %zu active mods (%s), %zu with keys, %d keys (%llu ms)", active.size(),
        source.empty() ? "none found" : source.c_str(), result.size(), keys, GetTickCount64() - t0);
    return result;
}

void Refresh()
{
    if (g_running.exchange(true)) return;
    std::wstring game = ModDirectory();
    std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    std::wstring appData = local.empty() ? std::wstring() : local + L"\\Teardown\\";
    std::wstring docs = KnownFolder(FOLDERID_Documents);
    try {
        std::thread([game, appData, docs] {
            std::vector<Mod> mods;
            try {
                if (!appData.empty()) mods = ScanNow(game, appData, docs);
            } catch (...) {
                Log("ModKeys: scan failed");
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_result = std::move(mods);
                ++g_serial;
            }
            g_running = false;
        }).detach();
    } catch (...) {
        g_running = false;
    }
}

bool Scanning() { return g_running; }

std::vector<Mod> Get(unsigned* serial)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (serial) *serial = g_serial;
    return g_result;
}

unsigned Serial()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_serial;
}

} // namespace ModKeys
