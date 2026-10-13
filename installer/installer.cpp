// Teardown VR - the players' installer: one .exe carrying the whole mod (the DLL and the Lua mod, as resources,
// tools/package_release.ps1 generates gen/payload.h and gen/installer_gen.rc from build\Release\dsound.dll and
// mod\TeardownVR). It does what tools/install.ps1 does, as a single .exe:
//   - asks first the language (English or French; pre-selected: English, or French when the game's settings are): the
//     one of every later window, and the mod's;
//   - finds Teardown through Steam (HKCU\Software\Valve\Steam SteamPath, HKLM WOW6432Node InstallPath, then every
//     library of steamapps\libraryfolders.vdf), else asks for the folder;
//   - refuses while teardown.exe runs (its dsound.dll is locked, and the mod would load half-copied);
//   - install / update: another mod's dsound.dll (no "TDVR1" in it) is kept as dsound.dll.original, then the files
//     are written (each to a temporary name, then moved over the old one); of TeardownVR.ini (the player's settings)
//     only [Affichage] language is written (the language chosen; the file is created if missing);
//   - uninstall: our dsound.dll removed (the other mod's put back), mods\teardownvr and TeardownVR.log removed, the
//     settings kept;
//   - Windows refuses to write in the game folder (a library under Program Files with other rights): it offers to
//     run again as administrator (with the language chosen);
//   - "Préparer mes journaux": a zip on the Desktop with the mod's logs, the settings and the PC's description, for
//     the player (logs.cpp, its infos_PC.txt in French, for bug reports; the game may be
//     running).
// No arguments: a window (task dialog). Tests: /install, /uninstall or /logs (with /out "<folder>" instead of the
// Desktop), /gamedir "<folder>", /lang en|fr (no language window; without it, /silent leaves the ini's language as
// it is), /silent (no window at all; exit code 0 = done, 2 = game running, 3 = no game folder, 5 = access denied,
// other = Windows error).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

#include "payload.h"
#include "logs.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr DWORD kExitGameRunning = 2, kExitNoGame = 3;
const wchar_t* const kTitle = L"Teardown VR";

// The language chosen in the first window or by /lang: "en" or "fr" (empty: /silent without /lang, no window and the
// ini's language left as it is). The windows' texts: T(L"English", L"Français").
std::wstring g_lang;
bool Fr() { return g_lang == L"fr"; }
const wchar_t* T(const wchar_t* en, const wchar_t* fr) { return Fr() ? fr : en; }

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring Join(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* name, DWORD view = 0)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | view, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool ReadAll(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64ll << 20);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// The Steam folder, then every library listed in its libraryfolders.vdf ("path"  "D:\\SteamLibrary").
std::vector<std::wstring> SteamLibraries()
{
    std::vector<std::wstring> libs;
    auto add = [&](std::wstring p) {
        for (wchar_t& c : p)
            if (c == L'/') c = L'\\';
        while (!p.empty() && p.back() == L'\\') p.pop_back();
        if (p.empty()) return;
        for (const std::wstring& q : libs)
            if (CompareStringOrdinal(q.c_str(), -1, p.c_str(), -1, TRUE) == CSTR_EQUAL) return;
        libs.push_back(p);
    };
    add(RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    add(RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", RRF_SUBKEY_WOW6432KEY));
    add(RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", RRF_SUBKEY_WOW6464KEY));
    const size_t steamRoots = libs.size();
    for (size_t i = 0; i < steamRoots; ++i) {
        std::string vdf;
        if (!ReadAll(Join(libs[i], L"steamapps\\libraryfolders.vdf"), vdf)) continue;
        for (size_t at = 0; (at = vdf.find("\"path\"", at)) != std::string::npos;) {
            at += 6;
            const size_t q1 = vdf.find('"', at);
            if (q1 == std::string::npos) break;
            std::string p;
            size_t k = q1 + 1;
            for (; k < vdf.size() && vdf[k] != '"'; ++k) {
                if (vdf[k] == '\\' && k + 1 < vdf.size()) ++k;   // ("D:\\SteamLibrary": escaped backslashes)
                p += vdf[k];
            }
            at = k;
            add(FromUtf8(p));
        }
    }
    return libs;
}

bool IsGameFolder(const std::wstring& dir) { return !dir.empty() && Exists(Join(dir, L"teardown.exe")); }

std::wstring FindTeardown()
{
    for (const std::wstring& lib : SteamLibraries()) {
        const std::wstring dir = Join(lib, L"steamapps\\common\\Teardown");
        if (IsGameFolder(dir)) return dir;
    }
    return {};
}

bool GameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = CompareStringOrdinal(pe.szExeFile, -1, L"teardown.exe", -1, TRUE) == CSTR_EQUAL;
    CloseHandle(snap);
    return found;
}

// Our DLL (any version) carries the bridge protocol's name "TDVR1"; another mod's dsound.dll does not.
bool IsOurDll(const std::wstring& path)
{
    std::string bytes;
    return ReadAll(path, bytes) && bytes.find("TDVR1") != std::string::npos;
}

DWORD WriteFileAtomic(const std::wstring& dest, const void* data, DWORD size)
{
    const size_t slash = dest.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        const int r = SHCreateDirectoryExW(nullptr, dest.substr(0, slash).c_str(), nullptr);
        if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) return (DWORD)r;
    }
    const std::wstring tmp = dest + L".tdvrnew";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, data, size, &wrote, nullptr) && wrote == size && FlushFileBuffers(h);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (ok && !MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        err = GetLastError();
    if (err != ERROR_SUCCESS) DeleteFileW(tmp.c_str());
    return err;
}

DWORD WritePayloadFile(const PayloadFile& f, const std::wstring& game)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(f.id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    const DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!data) return ERROR_RESOURCE_DATA_NOT_FOUND;
    return WriteFileAtomic(Join(game, f.path), data, size);
}

DWORD Install(const std::wstring& game, std::wstring& note)
{
    const std::wstring dll = Join(game, L"dsound.dll");
    if (Exists(dll) && !IsOurDll(dll)) {
        // (another mod's dsound.dll: kept aside, as tools/install.ps1 does)
        if (!CopyFileW(dll.c_str(), (dll + L".original").c_str(), FALSE)) return GetLastError();
        note = T(L"Another dsound.dll (from another mod) was already there: it is kept as dsound.dll.original.\n",
                 L"Une autre dsound.dll (d'un autre mod) existait : elle est gardée sous le nom dsound.dll.original.\n");
    }
    for (const PayloadFile& f : kPayload)
        if (const DWORD e = WritePayloadFile(f, game)) return e;
    // (the mod's language, read at start by src/config.cpp: that key only, the other settings are the player's)
    if (!g_lang.empty() &&
        !WritePrivateProfileStringW(L"Affichage", L"language", g_lang.c_str(), Join(game, L"TeardownVR.ini").c_str())) {
        const DWORD e = GetLastError();
        return e ? e : ERROR_WRITE_FAULT;
    }
    return ERROR_SUCCESS;
}

DWORD Uninstall(const std::wstring& game)
{
    const std::wstring dll = Join(game, L"dsound.dll");
    if (Exists(dll) && IsOurDll(dll) && !DeleteFileW(dll.c_str())) return GetLastError();
    const std::wstring orig = dll + L".original";
    if (Exists(orig) && !Exists(dll) && !MoveFileW(orig.c_str(), dll.c_str())) return GetLastError();
    // mods\teardownvr: the files this installer writes, then the folder if nothing else is left in it
    for (const PayloadFile& f : kPayload) {
        if (_wcsnicmp(f.path, L"mods\\teardownvr\\", 16) != 0) continue;
        const std::wstring p = Join(game, f.path);
        if (Exists(p) && !DeleteFileW(p.c_str())) return GetLastError();
    }
    RemoveDirectoryW(Join(game, L"mods\\teardownvr").c_str());
    DeleteFileW(Join(game, L"TeardownVR.log").c_str());
    // (the logs the mod kept, src/log.cpp: our files only, then the folder if nothing else is in it)
    const std::wstring logs = Join(game, L"TeardownVR_journaux");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(logs, L"TeardownVR_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(Join(logs, fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(logs.c_str());
    return ERROR_SUCCESS;
}

bool Elevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size);
    if (token) CloseHandle(token);
    return ok && e.TokenIsElevated;
}

std::wstring ErrorText(DWORD e)
{
    wchar_t* msg = nullptr;
    // (in the language chosen when Windows has it, else in Windows' own)
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD lang = Fr() ? MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH) : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    if (g_lang.empty() || !FormatMessageW(flags, nullptr, e, lang, (LPWSTR)&msg, 0, nullptr)) {
        msg = nullptr;
        FormatMessageW(flags, nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    }
    std::wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s + L" (code " + std::to_wstring(e) + L")";
}

// --- the window --------------------------------------------------------------------------------------------------
// custom: command links, or plain buttons (links false); defaultButton: the one Enter presses (0: the first).
int Dialog(HWND owner, const std::wstring& instruction, const std::wstring& content, PCWSTR icon,
           TASKDIALOG_COMMON_BUTTON_FLAGS buttons, const TASKDIALOG_BUTTON* custom = nullptr, UINT customCount = 0,
           const std::wstring& footer = {}, bool links = true, int defaultButton = 0)
{
    TASKDIALOGCONFIG c{ sizeof(c) };
    c.hwndParent = owner;
    c.hInstance = GetModuleHandleW(nullptr);
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT |
                (customCount && links ? TDF_USE_COMMAND_LINKS : 0) | TDF_USE_HICON_MAIN;
    c.nDefaultButton = defaultButton;
    c.dwCommonButtons = buttons;
    c.pszWindowTitle = kTitle;
    c.hMainIcon = icon ? nullptr : LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    if (icon) {
        c.dwFlags &= ~TDF_USE_HICON_MAIN;
        c.pszMainIcon = icon;
    }
    c.pszMainInstruction = instruction.c_str();
    c.pszContent = content.c_str();
    c.pButtons = custom;
    c.cButtons = customCount;
    if (!footer.empty()) {
        c.pszFooter = footer.c_str();
        c.pszFooterIcon = TD_INFORMATION_ICON;
    }
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) {
        // (no task dialog: a plain message box)
        const std::wstring text = instruction + L"\n\n" + content;
        return MessageBoxW(owner, text.c_str(), kTitle, MB_OKCANCEL | MB_ICONINFORMATION) == IDOK ? IDOK : IDCANCEL;
    }
    return pressed;
}

std::wstring PickFolder()
{
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(T(L"Choose the Teardown folder (the one with teardown.exe in it)",
                  L"Choisis le dossier de Teardown (celui qui contient teardown.exe)"));
    if (SUCCEEDED(d->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    d->Release();
    return out;
}

bool AskFolder(std::wstring& game)
{
    for (;;) {
        const std::wstring dir = PickFolder();
        if (dir.empty()) return false;
        if (IsGameFolder(dir)) {
            game = dir;
            return true;
        }
        Dialog(nullptr, T(L"This folder doesn't contain teardown.exe.", L"Ce dossier ne contient pas teardown.exe."),
               T(L"Choose the game folder, usually:\nC:\\Program Files (x86)\\Steam\\steamapps\\common\\Teardown",
                 L"Choisis le dossier du jeu, en général :\nC:\\Program Files (x86)\\Steam\\steamapps\\common\\Teardown"),
               TD_WARNING_ICON, TDCBF_OK_BUTTON);
    }
}

bool WaitGameClosed(bool silent)
{
    while (GameRunning()) {
        if (silent) return false;
        // (our own buttons, in the language chosen: the text names Retry, and Windows draws its own in its language)
        const TASKDIALOG_BUTTON buttons[] = {
            { IDRETRY, T(L"Retry", L"Réessayer") },
            { IDCANCEL, T(L"Cancel", L"Annuler") },
        };
        if (Dialog(nullptr, T(L"Close Teardown before continuing.", L"Ferme Teardown avant de continuer."),
                   T(L"The game is running: quit it, then click Retry.",
                     L"Le jeu est ouvert : quitte-le, puis clique sur Réessayer."),
                   TD_WARNING_ICON, 0, buttons, 2, std::wstring(), false) != IDRETRY)
            return false;
    }
    return true;
}

bool RelaunchAsAdmin(const std::wstring& action, const std::wstring& game)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = game;
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();   // (no \" in the command line)
    if (!dir.empty() && dir.back() == L'\\') dir += L'.';           // ("D:\" -> "D:\.")
    std::wstring args = L"/" + action + L" /gamedir \"" + dir + L"\"";
    if (!g_lang.empty()) args += L" /lang " + g_lang;   // (the language chosen: not asked again)
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

constexpr int kCmdInstall = 100, kCmdUninstall = 101, kCmdOtherFolder = 102, kCmdLogs = 103;
constexpr int kCmdLangEn = 110, kCmdLangFr = 111;

// The player's settings already in French, by the mod's rule (src/config.cpp ConfigLoad): the game's TeardownVR.ini
// says language=fr, or has no language (an ini from before the English version).
bool IniFrench(const std::wstring& game)
{
    const std::wstring ini = Join(game, L"TeardownVR.ini");
    if (!IsGameFolder(game) || !Exists(ini)) return false;
    wchar_t lang[16];
    return GetPrivateProfileStringW(L"Affichage", L"language", L"", lang, 16, ini.c_str()) == 0 || !wcscmp(lang, L"fr");
}

// The first window: the language (English first, the mod's default; Enter keeps French for a player whose settings in
// that game folder are French). False: closed without choosing.
bool AskLanguage(const std::wstring& game)
{
    const TASKDIALOG_BUTTON buttons[] = {
        { kCmdLangEn, L"English" },
        { kCmdLangFr, L"Français" },
    };
    const int cmd = Dialog(nullptr, L"Choose your language / Choisis ta langue", std::wstring(), nullptr, 0, buttons, 2,
                           std::wstring(), true, IniFrench(game) ? kCmdLangFr : kCmdLangEn);
    if (cmd != kCmdLangEn && cmd != kCmdLangFr) return false;
    g_lang = cmd == kCmdLangFr ? L"fr" : L"en";
    return true;
}

// "Préparer mes journaux" (the game may be running: the logs are only read).
DWORD RunLogs(const std::wstring& game, const std::wstring& outDir, bool silent)
{
    std::wstring zip;
    int count = 0;
    const DWORD e = CollectLogs(game, outDir, zip, count);
    if (silent) return e;
    if (e != ERROR_SUCCESS) {
        Dialog(nullptr, T(L"Your logs couldn't be prepared.", L"Les journaux n'ont pas pu être préparés."),
               ErrorText(e) + L"\n\n" + zip, TD_ERROR_ICON, TDCBF_CLOSE_BUTTON);
        return e;
    }
    const size_t slash = zip.find_last_of(L'\\');
    const std::wstring name = slash == std::wstring::npos ? zip : zip.substr(slash + 1);
    Dialog(nullptr, T(L"Your logs are ready!", L"Tes journaux sont prêts !"),
           T(L"The file \"", L"Le fichier « ") + name +
               T(L"\" is on your Desktop.\n\n"
                 L"It contains: the mod's logs (",
                 L" » est sur ton Bureau.\n\n"
                 L"Il contient : les journaux du mod (") +
               std::to_wstring(count) +
               T(L" recent session(s)), your VR settings and a description of your PC (Windows, processor, memory, "
                 L"graphics card). Nothing is sent automatically.",
                 L" partie(s) récente(s)), tes réglages VR et la description de ton PC (Windows, processeur, "
                 L"mémoire, carte graphique). Rien n'est envoyé automatiquement."),
           TD_INFORMATION_ICON, TDCBF_OK_BUTTON, nullptr, 0,
           count ? std::wstring()
                 : std::wstring(T(L"No logs found: launch the game once with the mod, then try again.",
                                  L"Aucun journal trouvé : lance le jeu une fois avec le mod, puis recommence.")));
    ShowInExplorer(zip);
    return ERROR_SUCCESS;
}

// Runs one action with its windows; returns the exit code.
DWORD RunAction(int cmd, const std::wstring& game, bool silent)
{
    if (!WaitGameClosed(silent)) return kExitGameRunning;
    std::wstring note;
    const DWORD e = cmd == kCmdInstall ? Install(game, note) : Uninstall(game);
    if (silent) return e;
    if (e == ERROR_ACCESS_DENIED && !Elevated()) {
        if (Dialog(nullptr, T(L"Windows won't allow writing to the game folder.", L"Windows refuse d'écrire dans le dossier du jeu."),
                   T(L"The Teardown folder is protected on this PC. Run again as administrator?\n\n",
                     L"Le dossier de Teardown est protégé sur ce PC. Relancer en administrateur ?\n\n") + game,
                   TD_SHIELD_ICON, TDCBF_YES_BUTTON | TDCBF_NO_BUTTON) == IDYES &&
            RelaunchAsAdmin(cmd == kCmdInstall ? L"install" : L"uninstall", game))
            return ERROR_SUCCESS;
        return e;
    }
    if (e != ERROR_SUCCESS) {
        Dialog(nullptr, cmd == kCmdInstall ? T(L"The installation couldn't finish.", L"L'installation n'a pas pu se terminer.")
                                           : T(L"The uninstallation couldn't finish.", L"La désinstallation n'a pas pu se terminer."),
               ErrorText(e) + T(L"\n\nGame folder: ", L"\n\nDossier du jeu : ") + game, TD_ERROR_ICON, TDCBF_CLOSE_BUTTON);
        return e;
    }
    if (cmd == kCmdInstall) {
        Dialog(nullptr, T(L"Teardown VR is installed!", L"Teardown VR est installé !"),
               note +
                   T(L"One last step, only once:\n"
                     L"  1. Launch Teardown.\n"
                     L"  2. Play > Mod manager > \"Built-in\" list > \"Teardown VR\" > click \"Disabled\" to "
                     L"enable it.\n"
                     L"  3. Start Virtual Desktop (or Quest Link) on your Quest 3, then a level.\n\n"
                     L"VR settings: long-press the Menu button on the left controller, or F10.\n"
                     L"Your settings (TeardownVR.ini) are kept when you update.",
                     L"Dernière étape, une seule fois :\n"
                     L"  1. Lance Teardown.\n"
                     L"  2. Jouer > Gestionnaire de mods > liste « Intégré » > « Teardown VR » > clique sur "
                     L"« Désactivé » pour l'activer.\n"
                     L"  3. Lance Virtual Desktop (ou Quest Link) sur ton Quest 3, puis un niveau.\n\n"
                     L"Réglages VR : appui long sur le bouton Menu de la manette gauche, ou F10.\n"
                     L"Tes réglages (TeardownVR.ini) sont conservés lors d'une mise à jour."),
               TD_INFORMATION_ICON, TDCBF_OK_BUTTON, nullptr, 0,
               std::wstring(T(L"Mod version: ", L"Version du mod : ")) + kPayloadVersion);
    } else {
        Dialog(nullptr, T(L"Teardown VR is uninstalled.", L"Teardown VR est désinstallé."),
               T(L"The game is back to normal. Your settings (TeardownVR.ini) stay in the game folder, for a future "
                 L"install.",
                 L"Le jeu redevient normal. Tes réglages (TeardownVR.ini) sont gardés dans le dossier du jeu, "
                 L"pour une prochaine installation."),
               TD_INFORMATION_ICON, TDCBF_OK_BUTTON);
    }
    return ERROR_SUCCESS;
}

int RunWindow()
{
    std::wstring game = FindTeardown();
    if (game.empty()) {
        if (Dialog(nullptr, T(L"Teardown wasn't found automatically.", L"Teardown n'a pas été trouvé automatiquement."),
                   T(L"Click OK, then choose the game folder (the one with teardown.exe in it).",
                     L"Clique sur OK puis choisis le dossier du jeu (celui qui contient teardown.exe)."),
                   TD_WARNING_ICON, TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON) != IDOK ||
            !AskFolder(game))
            return (int)kExitNoGame;
    }
    for (;;) {
        const bool installed = Exists(Join(game, L"dsound.dll")) && IsOurDll(Join(game, L"dsound.dll"));
        const TASKDIALOG_BUTTON buttons[] = {
            { kCmdInstall, installed ? T(L"Update Teardown VR\nReplaces the mod with this version. Your settings are kept.",
                                         L"Mettre à jour Teardown VR\nRemplace le mod par cette version. Tes réglages sont conservés.")
                                     : T(L"Install Teardown VR\nCopies the mod into the game folder.",
                                         L"Installer Teardown VR\nCopie le mod dans le dossier du jeu.") },
            { kCmdUninstall, T(L"Uninstall Teardown VR\nRemoves the mod: the game is back to normal (your settings are kept).",
                               L"Désinstaller Teardown VR\nRetire le mod : le jeu redevient normal (tes réglages sont gardés).") },
            { kCmdLogs, T(L"Prepare my logs\nCreates a zip file on your Desktop with the mod's logs and a "
                          L"description of your PC.",
                          L"Préparer mes journaux\nCrée sur le Bureau un fichier zip avec les journaux du mod et la "
                          L"description de ton PC.") },
            { kCmdOtherFolder, T(L"Choose another game folder", L"Choisir un autre dossier du jeu") },
        };
        const int cmd = Dialog(nullptr, T(L"Teardown VR: virtual reality for Teardown", L"Teardown VR : la réalité virtuelle pour Teardown"),
                               (installed ? T(L"Teardown VR is already installed in:\n", L"Teardown VR est déjà installé dans :\n")
                                          : T(L"Game found:\n", L"Jeu trouvé :\n")) + game,
                               nullptr, TDCBF_CANCEL_BUTTON, buttons, 4,
                               std::wstring(T(L"Mod version: ", L"Version du mod : ")) + kPayloadVersion);
        if (cmd == kCmdOtherFolder) {
            AskFolder(game);
            continue;
        }
        if (cmd == kCmdLogs) return (int)RunLogs(game, std::wstring(), false);
        if (cmd != kCmdInstall && cmd != kCmdUninstall) return 1;
        return (int)RunAction(cmd, game, false);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int cmd = 0;
    bool silent = false;
    std::wstring game, outDir;
    for (int i = 1; argv && i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"/install")) cmd = kCmdInstall;
        else if (!_wcsicmp(argv[i], L"/uninstall")) cmd = kCmdUninstall;
        else if (!_wcsicmp(argv[i], L"/logs")) cmd = kCmdLogs;
        else if (!_wcsicmp(argv[i], L"/out") && i + 1 < argc) outDir = argv[++i];
        else if (!_wcsicmp(argv[i], L"/silent")) silent = true;
        else if (!_wcsicmp(argv[i], L"/gamedir") && i + 1 < argc) game = argv[++i];
        else if (!_wcsicmp(argv[i], L"/lang") && i + 1 < argc) {
            ++i;   // (en or fr; anything else: as if not given)
            if (!_wcsicmp(argv[i], L"en") || !_wcsicmp(argv[i], L"fr")) g_lang = _wcsicmp(argv[i], L"fr") ? L"en" : L"fr";
        }
    }
    if (argv) LocalFree(argv);
    int code;
    if ((!cmd || !silent) && g_lang.empty() && !AskLanguage(game.empty() ? FindTeardown() : game)) {
        code = 1;   // (the language window closed: nothing done)
    } else if (cmd) {
        if (game.empty()) game = FindTeardown();
        if (!IsGameFolder(game)) {
            if (!silent)
                Dialog(nullptr, T(L"Game folder not found.", L"Dossier du jeu introuvable."),
                       game.empty() ? std::wstring(T(L"(none)", L"(aucun)")) : game, TD_ERROR_ICON, TDCBF_CLOSE_BUTTON);
            code = (int)kExitNoGame;
        } else {
            code = (int)(cmd == kCmdLogs ? RunLogs(game, outDir, silent) : RunAction(cmd, game, silent));
        }
    } else {
        code = RunWindow();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return code;
}
