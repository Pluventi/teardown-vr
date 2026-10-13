#include "config.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

const RenderPreset kRenderPresets[] = {
    { "PC screen (no adjustment)", "Écran du PC (sans ajustement)", 0 },
    { "Low", "Basse", 1280 },
    { "Medium", "Moyenne", 1600 },
    { "High", "Haute", 1920 },
    { "Very high", "Très haute", 2272 },
    { "Ultra", "Ultra", 2720 },
    { "Headset native (Quest 3)", "Native du casque (Quest 3)", 3264 },   // = résolution recommandée par Virtual Desktop
};
const int kRenderPresetCount = sizeof(kRenderPresets) / sizeof(kRenderPresets[0]);

static Settings g_settings;

Settings& Cfg() { return g_settings; }

const char* RenderPresetName(int preset)
{
    const RenderPreset& p = kRenderPresets[preset < 0 || preset >= kRenderPresetCount ? 0 : preset];
    return g_settings.language == "fr" ? p.nameFr : p.nameEn;
}

void RenderPresetSize(int preset, int& w, int& h, float coverage)
{
    const RenderPreset& p = kRenderPresets[preset < 0 || preset >= kRenderPresetCount ? 0 : preset];
    float aspect = g_settings.vrAspect > 0.5f && g_settings.vrAspect < 2.5f ? g_settings.vrAspect : 1.15f;
    // (fovCrop: only the covered part of the field, at the preset's pixel density; 1 = the preset itself, its
    // lines being multiples of 8)
    const float c = coverage > 0.05f && coverage < 1.0f ? coverage : 1.0f;
    h = p.lines ? ((int)(p.lines * c + 0.5f) + 7) / 8 * 8 : 0;
    w = h ? ((int)(h * aspect) + 7) / 8 * 8 : 0;
}

float FovCoverage()
{
    const float c = g_settings.fovCoverage;
    if (!g_settings.fovCrop) return std::clamp(c, 0.3f, 1.5f);   // (as before)
    return c > 0 ? std::clamp(c, 0.5f, 1.0f) : 1.0f;   // (0, negative or NaN from a hand-edited ini: all of it)
}

float RenderCoverage() { return g_settings.fovCrop ? FovCoverage() : 1.0f; }

std::wstring ModDirectory()
{
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        dir = path;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    }
    return dir;
}

static std::wstring IniPath() { return ModDirectory() + L"TeardownVR.ini"; }

namespace {
struct Field { const char* section; const char* key; char type; void* ptr; };

std::vector<Field> Fields(Settings& s)
{
    std::vector<Field> f;

#define F_(sec, name, t) f.push_back(Field{ sec, #name, t, &s.name })
    F_("Affichage", language, 's');
    F_("Affichage", renderPreset, 'i');
    F_("Affichage", vrAspect, 'f');
    F_("Affichage", fovCoverage, 'f');
    F_("Affichage", fovCrop, 'b');
    F_("Affichage", eyeResScale, 'f');
    F_("Affichage", refreshRate, 'i');
    F_("Affichage", stereoMode, 'i');
    F_("Affichage", perEyeHistory, 'b');
    F_("Affichage", stereoSameNoise, 'i');
    F_("Affichage", stereoParticles, 'b');
    F_("Affichage", stereoParticleMotion, 'i');
    F_("Affichage", stereoParity, 'i');
    F_("Affichage", removeFog, 'b');
    F_("Affichage", removeBloom, 'b');
    F_("Affichage", removeDof, 'b');
    F_("Affichage", headsetRuntime, 'i');
    F_("Affichage", taaMode, 'i');
    F_("Affichage", offCenter, 'b');
    F_("Affichage", syncParticles, 'b');
    F_("Affichage", syncScriptMotion, 'b');
    F_("Affichage", limitCatchUp, 'b');
    F_("Affichage", ipdScale, 'f');
    F_("Affichage", worldScale, 'f');
    F_("Affichage", sharpen, 'f');
    F_("Affichage", brightness, 'f');
    F_("Affichage", hideHud, 'b');
    F_("Affichage", hudFix, 'b');
    F_("Affichage", hudDistance, 'f');
    F_("Affichage", modCrosshairAim, 'b');
    F_("Affichage", modUiPointer, 'b');
    F_("Affichage", modUiDistance, 'f');
    F_("Affichage", modUiCenterTool, 'b');
    F_("Affichage", modUiToolFixed, 'b');
    F_("Affichage", modUiPanel, 'b');
    F_("Affichage", modUiPanelScale, 'f');
    F_("Affichage", modUiPanelPlaces, 's');
    F_("Affichage", modCameraVr, 'b');
    F_("Affichage", modCameraDepth, 'f');
    F_("Affichage", modCameraZoom, 'b');
    F_("Affichage", modZoomMode, 'i');
    F_("Affichage", laserPointer, 'b');
    F_("Confort", turnMode, 'i');
    F_("Confort", snapAngle, 'f');
    F_("Confort", smoothTurnSpeed, 'f');
    F_("Confort", moveDirection, 'i');
    F_("Confort", moveSpeed, 'f');
    F_("Confort", roomscale, 'b');
    F_("Confort", heightOffset, 'f');
    F_("Confort", physicalHeight, 'b');
    F_("Confort", vignette, 'b');
    F_("Confort", vignetteStrength, 'f');
    F_("Controles", leftHanded, 'b');
    F_("Controles", aimMode, 'i');
    F_("Controles", modAimHand, 'b');
    F_("Controles", dualWield, 'b');
    F_("Controles", haptics, 'b');
    F_("Controles", offArm, 'b');
    F_("Controles", offHandPhysics, 'b');
    F_("Controles", offGrab, 'b');
    F_("Controles", offHandPointer, 'b');
    F_("Controles", offLaser, 'b');
    F_("Controles", grabKeepDistance, 'b');
    F_("Controles", grabMinDistance, 'f');
    F_("Controles", grabStickSpeed, 'f');
    F_("Controles", inventoryOnSwitch, 'b');
    F_("Controles", inventoryDown, 'f');
    F_("Controles", inventoryForward, 'f');
    F_("Controles", inventoryYaw, 'f');
    F_("Controles", inventoryFollow, 'b');
    F_("Controles", inventoryFollowAngle, 'f');
    F_("Controles", inventoryFollowDelay, 'f');
    F_("Controles", swingGesture, 'b');
    F_("Controles", sledgeTap, 'b');
    F_("Controles", sledgeTrigger, 'f');
    F_("Controles", toolCollision, 'b');
    F_("Controles", toolPhysics, 'b');
    F_("Controles", modToolScale, 'f');
    F_("Controles", toolBlock, 'b');
    F_("Controles", toolOffsetX, 'f');
    F_("Controles", toolOffsetY, 'f');
    F_("Controles", toolOffsetZ, 'f');
    F_("Controles", toolPitch, 'f');
    F_("Mods", modMouse, 'i');
    F_("Mods", modMouseStick, 'i');
    F_("Mods", modMouseSpeed, 'f');
    F_("Mods", quickKey, 's');
    F_("Ecran", screenDistance, 'f');
    F_("Ecran", screenWidth, 'f');
    F_("Ecran", forceScreen, 'b');
    F_("Ecran", pauseWide, 'b');
    F_("Interface", uiScale, 'f');
    F_("Interface", uiPanelDistance, 'f');
    F_("Interface", uiPanelDown, 'f');
    F_("Interface", showDesktopUi, 'b');
    F_("Interface", gpuWatch, 'b');
    F_("Debug", debugFakeHmd, 'b');
    F_("Debug", debugLua, 'b');
    F_("Debug", particleMotionLog, 'b');
    F_("Debug", debugSyncTest, 'b');
    F_("Debug", debugWatchCamera, 'b');
    F_("Debug", debugGpuTrace, 'b');
    F_("Debug", debugFovTest, 'b');
    F_("Debug", debugNoAim, 'b');
    F_("Debug", debugProfile, 'b');
    F_("Debug", perfTrace, 'i');
    F_("Debug", logSync, 'b');
    F_("Performance", cpuPriority, 'i');
    F_("Performance", fastToolShapes, 'b');
    F_("Performance", fastToolScaleB, 'b');
    F_("Performance", fastObjectMotion, 'b');
    F_("Performance", fastLua, 'b');
    F_("Performance", frameBMode, 'i');
    F_("Performance", frameLatency, 'i');
    F_("Performance", lazyBeginFrame, 'i');
    F_("Performance", holdSmooth, 'b');
    F_("Performance", frameBKeepMods, 's');
    F_("Performance", jobSpin, 'i');
    F_("Performance", jobSpinUs, 'i');
    F_("Performance", mainSpinUs, 'i');
    F_("Performance", hiddenAreaFill, 'i');
    F_("Performance", hiddenAreaGuard, 'f');
    F_("Performance", gpuPriority, 'i');
    F_("Performance", vdStats, 'b');
    F_("Performance", parallelParticleSort, 'i');
    F_("Performance", skipShapeJobsB, 'i');
    F_("Performance", skipDeadPasses, 'i');
    F_("Performance", perfDetail, 'i');
#undef F_
    return f;
}

std::string FieldText(const Field& f)
{
    char buf[64];
    switch (f.type) {
    case 'i': snprintf(buf, sizeof(buf), "%d", *(int*)f.ptr); break;
    case 'f': snprintf(buf, sizeof(buf), "%.3f", *(float*)f.ptr); break;
    case 'b': snprintf(buf, sizeof(buf), "%d", *(bool*)f.ptr ? 1 : 0); break;
    case 's': return *(std::string*)f.ptr;   // (whole: modUiPanelPlaces is longer than 64)
    default: buf[0] = 0; break;
    }
    return buf;
}

std::string IniPathA()
{
    char path[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, IniPath().c_str(), -1, path, MAX_PATH, nullptr, nullptr);
    return path;
}

// One writer of the ini at a time: each WritePrivateProfileString reads and rewrites the whole
// file, two threads at once could lose a key (ConfigSave on the present thread, ConfigSaveKeys'
// worker).
std::mutex g_iniMutex;
// ConfigSaveKeys: the keys waiting for the worker thread (section, key, value; one entry per key,
// the latest value), whether that thread runs, and when it last wrote (GetTickCount64).
std::mutex g_keysMutex;
std::vector<std::array<std::string, 3>> g_keysPending;
bool g_keysBusy = false;
std::atomic<ULONGLONG> g_keysLast{ 0 };
// A full ConfigSave since a batch was taken: that batch is older than what ConfigSave wrote (it
// writes every key's current value) - the worker drops it instead of writing stale panel keys
// over newer values ("Distance et hauteur d'origine", "Réinitialiser tous les réglages").
std::atomic<unsigned> g_saveGen{ 0 };

void SaveKeysThread()
{
    for (;;) {
        // (at most once per second: a key saved again meanwhile waits, merged with the others)
        const ULONGLONG since = GetTickCount64() - g_keysLast.load();
        if (since < 1000) Sleep((DWORD)(1000 - since));
        std::vector<std::array<std::string, 3>> batch;
        unsigned gen;
        {
            std::lock_guard<std::mutex> lock(g_keysMutex);
            if (g_keysPending.empty()) {
                g_keysBusy = false;
                return;
            }
            batch.swap(g_keysPending);
            gen = g_saveGen;
        }
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const std::string path = IniPathA();
        std::string names;
        bool stale;
        {
            std::lock_guard<std::mutex> lock(g_iniMutex);
            // (checked under the ini's lock: a ConfigSave that bumped it writes after this, or
            // already wrote newer values)
            stale = g_saveGen != gen;
            for (const auto& k : batch) {
                if (!stale) WritePrivateProfileStringA(k[0].c_str(), k[1].c_str(), k[2].c_str(), path.c_str());
                names += (names.empty() ? "" : ", ") + k[1] + "=" + k[2];
            }
        }
        QueryPerformanceCounter(&t1);
        g_keysLast = GetTickCount64();
        static int logs = 0;
        if (logs < 100) {
            ++logs;
            if (stale)
                Log("Config: %s not written - the whole ini was saved since (newer values)", names.c_str());
            else
                Log("Config: saved %s (worker thread, %.1f ms)", names.c_str(),
                    double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
        }
    }
}
} // namespace

static bool g_xrForced = false;          // [Debug] openxrRuntime set: that runtime, no detection
static std::string g_xrAutoTest;         // [Debug] xrAutoRuntimes

bool ConfigXrAutoDetect()
{
    const int c = g_settings.headsetRuntime;
    return !g_xrForced && (c < 1 || c > 4);
}

const std::string& ConfigXrAutoTestRuntimes() { return g_xrAutoTest; }

void ConfigLoad()
{
    std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("Config: no ini yet, using defaults");
        ConfigSave();
        return;
    }
    char path[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, ini.c_str(), -1, path, MAX_PATH, nullptr, nullptr);
    for (const Field& f : Fields(g_settings)) {
        char buf[1024];   // (modUiPanelPlaces: up to 16 places of ~30 characters)
        DWORD len = GetPrivateProfileStringA(f.section, f.key, "", buf, sizeof(buf), path);
        if (len == 0) continue;
        switch (f.type) {
        case 'i': *(int*)f.ptr = atoi(buf); break;
        case 'f': *(float*)f.ptr = (float)atof(buf); break;
        case 'b': *(bool*)f.ptr = atoi(buf) != 0; break;
        case 's': *(std::string*)f.ptr = buf; break;
        }
    }
    if (g_settings.renderPreset < 0 || g_settings.renderPreset >= kRenderPresetCount)
        g_settings.renderPreset = 0;
    // [Affichage] language: an ini from before the English version (no such key) belongs to a French player; the
    // installer writes the key for a new one. Anything but "fr" is English.
    {
        char lang[16];
        if (GetPrivateProfileStringA("Affichage", "language", "", lang, sizeof(lang), path) == 0) {
            g_settings.language = "fr";
            {
                std::lock_guard<std::mutex> lock(g_iniMutex);
                WritePrivateProfileStringA("Affichage", "language", "fr", path);
            }
            Log("Config: [Affichage] language missing (an ini from before the English version) - French kept");
        } else if (g_settings.language != "fr") {
            g_settings.language = "en";
        }
    }
    // [Debug] openxrRuntime=<runtime .json>: use this OpenXR runtime instead of
    // the system's active one (read by the OpenXR loader at the first XR call).
    char runtime[512];
    g_xrForced = GetPrivateProfileStringA("Debug", "openxrRuntime", "", runtime, sizeof(runtime), path) > 0;
    if (g_xrForced) {
        SetEnvironmentVariableA("XR_RUNTIME_JSON", runtime);
        Log("Config: OpenXR runtime forced to %s", runtime);
    } else if (g_settings.headsetRuntime >= 1 && g_settings.headsetRuntime <= 3) {
        // [Affichage] headsetRuntime 1-3: this game's headset runtime, whatever Windows' active OpenXR runtime is
        // (Virtual Desktop's streamer makes itself the active one again while it runs, so switching it in
        // the Meta app was not enough for Quest Link). 0: chosen when the game starts (vr.cpp, RuntimeStartup:
        // never here, inside DllMain); 4: Windows' active runtime.
        const std::string json = ConfigFindXrRuntime(g_settings.headsetRuntime);
        if (!json.empty()) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", json.c_str());
            Log("Config: OpenXR runtime %s (headsetRuntime=%d)", json.c_str(), g_settings.headsetRuntime);
        } else {
            Log("Config: headsetRuntime=%d: that runtime is not installed - Windows' active OpenXR runtime is used",
                g_settings.headsetRuntime);
        }
    }
    // [Debug] xrAutoRuntimes=<a.json>;<b.json>...: a test of the automatic choice (headsetRuntime 0) asks these
    // runtimes instead of the real ones (the mock: tdvr_mock_openxr_nohmd.json;tdvr_mock_openxr.json)
    char autoTest[2048];
    GetPrivateProfileStringA("Debug", "xrAutoRuntimes", "", autoTest, sizeof(autoTest), path);
    g_xrAutoTest = autoTest;
    // The old key [Affichage] xrRuntime (0 = Windows' runtime, 1-3 forced) is replaced by headsetRuntime, whose
    // default is the automatic choice (asked by the player on 2026-10-12): its value is not carried over.
    char oldRuntime[16];
    if (GetPrivateProfileStringA("Affichage", "xrRuntime", "", oldRuntime, sizeof(oldRuntime), path) > 0) {
        {
            std::lock_guard<std::mutex> lock(g_iniMutex);
            WritePrivateProfileStringA("Affichage", "xrRuntime", nullptr, path);
        }
        Log("Config: [Affichage] xrRuntime=%s removed - replaced by headsetRuntime=%d (0 = automatic headset detection)",
            oldRuntime, g_settings.headsetRuntime);
    }
    if (g_settings.debugLua) {
        // OpenXR loader messages (it writes them to stderr) -> TeardownVR_xrloader.log
        SetEnvironmentVariableA("XR_LOADER_DEBUG", "info");
        std::wstring f = ModDirectory() + L"TeardownVR_xrloader.log";
        FILE* unused = nullptr;
        _wfreopen_s(&unused, f.c_str(), L"w", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    Log("Config: loaded %s", path);
}

int ConfigXrRuntimeKind(const std::string& json)
{
    static const char* const kMatch[4][2] = { { nullptr, nullptr }, { "virtual desktop", "virtualdesktop" },
                                               { "oculus", "meta horizon" }, { "steamvr", "steamxr" } };
    std::string lower = json;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    for (int k = 1; k <= 3; ++k)
        for (const char* m : kMatch[k])
            if (m && lower.find(m) != std::string::npos) return k;
    return 0;
}

std::string ConfigActiveXrRuntime()
{
    // (HKLM only, as the loader: third_party/openxr manifest_file.cpp ReadRuntimeDataFilesInRegistry)
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS)
        return {};
    char json[1024];
    DWORD size = sizeof(json);
    const LSTATUS r = RegGetValueA(key, nullptr, "ActiveRuntime", RRF_RT_REG_SZ, nullptr, json, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || GetFileAttributesA(json) == INVALID_FILE_ATTRIBUTES) return {};
    return json;
}

std::string ConfigFindXrRuntime(int choice)
{
    // The runtimes list themselves under AvailableRuntimes (value name = the .json path).
    if (choice < 1 || choice > 3) return {};
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return {};
    std::string found;
    for (DWORD i = 0; found.empty(); ++i) {
        char name[MAX_PATH];
        DWORD len = sizeof(name), type = 0;
        if (RegEnumValueA(key, i, name, &len, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::string json(name, len);
        if (ConfigXrRuntimeKind(json) == choice && GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES) found = json;
    }
    RegCloseKey(key);
    return found;
}

void ConfigSave()
{
    {
        // (every key's current value is written below: the batches still waiting are older)
        std::lock_guard<std::mutex> lock(g_keysMutex);
        g_keysPending.clear();
        ++g_saveGen;
    }
    const std::string path = IniPathA();
    std::lock_guard<std::mutex> lock(g_iniMutex);
    for (const Field& f : Fields(g_settings))
        WritePrivateProfileStringA(f.section, f.key, FieldText(f).c_str(), path.c_str());
}

void ConfigSaveKeys(std::initializer_list<const char*> keys)
{
    // (the values now, on the calling thread: the worker writes this copy)
    std::vector<std::array<std::string, 3>> now;
    for (const Field& f : Fields(g_settings))
        for (const char* k : keys)
            if (strcmp(f.key, k) == 0) now.push_back({ f.section, f.key, FieldText(f) });
    std::lock_guard<std::mutex> lock(g_keysMutex);
    for (auto& n : now) {
        auto it = std::find_if(g_keysPending.begin(), g_keysPending.end(),
                               [&](const std::array<std::string, 3>& p) { return p[1] == n[1]; });
        if (it != g_keysPending.end()) *it = n;
        else g_keysPending.push_back(n);
    }
    if (g_keysBusy || g_keysPending.empty()) return;
    g_keysBusy = true;
    std::thread(SaveKeysThread).detach();
}

void ConfigReset()
{
    const std::string language = g_settings.language;   // (a reset keeps the player's language)
    g_settings = Settings{};
    g_settings.language = language;
    ConfigSave();
}
