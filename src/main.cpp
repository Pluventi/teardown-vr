// Teardown VR - entry point.
//
// This DLL is loaded by teardown.exe as "dsound.dll". At process start it
// installs its hooks (clipboard bridge for the Lua mod, DXGI for rendering,
// raw input for the controllers); everything else happens lazily on the first
// Present call, once the game has created its D3D12 device.
#include "bridge.h"
#include "config.h"
#include "d3d_hooks.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "prof.h"
#include "vr.h"

#include <windows.h>
#include <algorithm>

static bool IsTeardown()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"teardown.exe") == 0;
}

static bool g_attached = false;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (!IsTeardown()) return TRUE; // e.g. teardown_modtest.exe: plain dsound proxy
        g_attached = true;
        LogInit();
        Log("Teardown VR loaded");
        ConfigLoad();
        // (the log's writer thread from here on, unless [Debug] logSync=1)
        LogConfigure(!Cfg().logSync);
        g_perfTrace = std::clamp(Cfg().perfTrace, 0, 2);
        if (g_perfTrace || Cfg().logSync)
            Log("Config: perfTrace %d, log written %s", g_perfTrace, Cfg().logSync ? "line by line (logSync)" : "by its thread");
        HooksInit();
        BridgeInit(VrHandleLuaRequest);
        Input::InstallHooks();
        FixFullscreenOption();
        D3DHooksInit();
    } else if (reason == DLL_PROCESS_DETACH && g_attached) {
        LogShutdown();   // (the lines still waiting for the log's writer thread)
    }
    return TRUE;
}
