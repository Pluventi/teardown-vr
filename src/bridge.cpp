#include "bridge.h"
#include "engine.h"
#include "hooks.h"
#include "input.h"
#include "inventory.h"
#include "log.h"
#include "perf.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using PFN_OpenClipboard    = BOOL(WINAPI*)(HWND);
using PFN_CloseClipboard   = BOOL(WINAPI*)();
using PFN_EmptyClipboard   = BOOL(WINAPI*)();
using PFN_GetClipboardData = HANDLE(WINAPI*)(UINT);
using PFN_SetClipboardData = HANDLE(WINAPI*)(UINT, HANDLE);

PFN_OpenClipboard    Real_OpenClipboard;
PFN_CloseClipboard   Real_CloseClipboard;
PFN_EmptyClipboard   Real_EmptyClipboard;
PFN_GetClipboardData Real_GetClipboardData;
PFN_SetClipboardData Real_SetClipboardData;

LuaHandler g_handler = nullptr;

// Virtual clipboard state (all calls come from the game's script thread).
HWND    g_openHwnd = nullptr;
bool    g_realOpen = false;
bool    g_pendingEmpty = false;
bool    g_responseArmed = false;
bool    g_responseConsumed = false;
HGLOBAL g_respA = nullptr, g_respW = nullptr;
int     g_requestCount = 0;

// Diagnostics: the last clipboard calls, dumped when the Lua side lost a request.
struct Trace { DWORD tid; char ev; char text[24]; };
Trace g_trace[32];
unsigned g_traceN = 0;
void T(char ev, const char* text = "")
{
    Trace& t = g_trace[g_traceN++ % 32];
    t.tid = GetCurrentThreadId();
    t.ev = ev;
    strncpy_s(t.text, text, _TRUNCATE);
}

bool EnsureRealOpen()
{
    if (g_realOpen) return true;
    if (!Real_OpenClipboard(g_openHwnd)) return false;
    g_realOpen = true;
    if (g_pendingEmpty) {
        Real_EmptyClipboard();
        g_pendingEmpty = false;
    }
    return true;
}

HGLOBAL MakeGlobal(const void* data, size_t bytes)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return nullptr;
    memcpy(GlobalLock(h), data, bytes);
    GlobalUnlock(h);
    return h;
}

// Returns true if the clipboard payload was a VR request (and was consumed).
bool HandleRequest(const std::string& text)
{
    // (the script ticking now is our Lua mod: frame B mode 1 always ticks it again, perf.h; not an
    // old copy of it, TDVR1, answered "VR inactive" below)
    if (text.compare(0, 4, "TDVR") == 0 && text.compare(0, 5, "TDVR1") != 0) Perf::NoteOurScript();
    // "TDVRL <text>": a diagnostic line from the Lua side, no response.
    // "TDVRY <degrees>": turn the player (aim), see EngineSetPlayerYaw.
    if (text.compare(0, 5, "TDVRY") == 0) {
        float yaw = 0;
        if (sscanf(text.c_str() + 5, "%f", &yaw) == 1) EngineSetPlayerYaw(yaw);
        return true;
    }
    // "TDVRT <x> <y> <z>": the laser target relative to this frame's eye camera (mods' crosshairs).
    if (text.compare(0, 5, "TDVRT") == 0) {
        Vec3 t;
        if (sscanf(text.c_str() + 5, "%f %f %f", &t.x, &t.y, &t.z) == 3 && std::abs(t.x) < 1e4f &&
            std::abs(t.y) < 1e4f && std::abs(t.z) < 1e4f)
            EngineSetAimTarget(t);
        return true;
    }
    // "TDVRZ <metres>": distance along a mod camera's axis to the point it aims at (0 =
    // nothing hit): its zoom moves the camera toward that point (engine.cpp, DollyTravel).
    if (text.compare(0, 5, "TDVRZ") == 0) {
        float d = 0;
        if (sscanf(text.c_str() + 5, "%f", &d) == 1) EngineSetModAimDistance(d);
        return true;
    }
    // "TDVRW <hand> <forearm>": the bodies of the hand that holds no tool (main.lua,
    // tdvrOffArmTick); "TDVRE <x> <y> <z>": the eye camera they were placed for in this
    // frame, minus the hand body's position (world). See engine.cpp, OffArmLateFix.
    if (text.compare(0, 5, "TDVRW") == 0) {
        int hand = 0, arm = 0;
        if (sscanf(text.c_str() + 5, "%d %d", &hand, &arm) == 2) EngineSetOffArm(hand, arm);
        return true;
    }
    if (text.compare(0, 5, "TDVRE") == 0) {
        Vec3 c;
        if (sscanf(text.c_str() + 5, "%f %f %f", &c.x, &c.y, &c.z) == 3 && std::abs(c.x) < 10 && std::abs(c.y) < 10 &&
            std::abs(c.z) < 10)
            EngineSetOffArmCamera(c);
        return true;
    }
    // Dual wield (main.lua, tdvrDualTick; engine.cpp, DualEye). Guns: 1 = dominant hand, 0 = the other.
    // "TDVRG <gun> <x> <y> <z>": its muzzle in its body; "TDVRK <gun> <x> <y> <z>": its laser target
    // minus that muzzle (world); "TDVRV <body>": the off hand's gun copy (0 = none); "TDVRX <x> <y> <z>":
    // the eye camera that copy was placed for in this frame, minus its position (late fix, like TDVRE).
    if (text.compare(0, 5, "TDVRG") == 0 || text.compare(0, 5, "TDVRK") == 0) {
        int gun = -1;
        Vec3 v;
        if (sscanf(text.c_str() + 5, "%d %f %f %f", &gun, &v.x, &v.y, &v.z) == 4 && std::abs(v.x) < 1e4f &&
            std::abs(v.y) < 1e4f && std::abs(v.z) < 1e4f)
            EngineSetDualGun(gun, text[4] == 'G' ? 0 : 1, v);
        return true;
    }
    if (text.compare(0, 5, "TDVRV") == 0) {
        int body = 0;
        if (sscanf(text.c_str() + 5, "%d", &body) == 1) EngineSetDualCopy(body);
        return true;
    }
    if (text.compare(0, 5, "TDVRX") == 0) {
        Vec3 c;
        if (sscanf(text.c_str() + 5, "%f %f %f", &c.x, &c.y, &c.z) == 3 && std::abs(c.x) < 10 && std::abs(c.y) < 10 &&
            std::abs(c.z) < 10)
            EngineSetDualCopyCamera(c);
        return true;
    }
    // "TDVRH <0/1> [<0/1>]": an object the physical off hand can take is within its reach, or it
    // holds one (main.lua, tdvrOffHandTick); the second field: right under the hand, not only
    // under its laser. (Its grip used to grab only then and crouch otherwise; it now always grabs -
    // input.cpp. This feeds the log, and keeps that grip from moving the VR inventory while the hand
    // is on an object - Input::OffHandTouching - or a mod's menu panel while an object is right under
    // it - Input::OffHandNear, vr.cpp.)
    if (text.compare(0, 5, "TDVRH") == 0) {
        int reach = 0, underHand = 0;
        const int n = sscanf(text.c_str() + 5, "%d %d", &reach, &underHand);
        if (n >= 1) Input::SetOffHandReach(reach != 0, n == 2 && underHand != 0);
        return true;
    }
    // "TDVRU": the game sees a movement key held while the player walks with the stick
    // only (main.lua, neutralizeGameWalk): release the movement keys.
    if (text.compare(0, 5, "TDVRU") == 0) {
        Input::RequestReleaseMovementKeys();
        return true;
    }
    // "TDVRS": hammer swing gesture.
    if (text.compare(0, 5, "TDVRS") == 0) {
        Input::RequestSwing();
        return true;
    }
    // "TDVRA <degrees>": yaw of the VR room in the game world this frame.
    if (text.compare(0, 5, "TDVRA") == 0) {
        float yaw = 0;
        if (sscanf(text.c_str() + 5, "%f", &yaw) == 1) EngineSetAnchorYaw(yaw);
        return true;
    }
    // "TDVRI...": the tool list for the VR inventory, in short pieces (see inventory.h).
    if (text.compare(0, 5, "TDVRI") == 0) {
        Inventory::HandleLuaMessage(text);
        return true;
    }
    if (text.compare(0, 5, "TDVRL") == 0) {
        Log("Lua: %s", text.c_str() + (text.size() > 6 ? 6 : text.size()));
        return true;
    }
    // An old copy of this mod (protocol TDVR1 - e.g. a first version left in
    // Documents\Teardown\mods and enabled as "local-TeardownVR" next to the installed one) ran
    // in parallel: it set the camera, the aim, the tool and the player's velocity (7 m/s with
    // the stick, also while the stick aims a mod camera) against this one, and each of its
    // requests counted as one of this frame's. It is answered "VR inactive", so it does nothing.
    if (text.compare(0, 5, "TDVR1") == 0) {
        static int logs = 0;
        if (logs < 3) {
            ++logs;
            Log("Bridge: an OLD copy of the Teardown VR mod is running (protocol TDVR1, e.g. Documents\\Teardown\\mods\\TeardownVR "
                "enabled as 'local-TeardownVR') - ignored; it should be disabled in the mod manager");
        }
        static const char kOff[] = "TDVR1 0 0";
        if (g_respA) GlobalFree(g_respA);
        if (g_respW) GlobalFree(g_respW);
        g_respA = MakeGlobal(kOff, sizeof(kOff));
        std::wstring w(kOff, kOff + sizeof(kOff) - 1);
        g_respW = MakeGlobal(w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        g_responseArmed = true;
        g_responseConsumed = false;
        return true;
    }
    if (text.compare(0, 5, "TDVR2") != 0) return false;

    LuaRequest req;
    sscanf(text.c_str() + 5, "%d %d %f %f %f", &req.seq, &req.flags, &req.hapticL, &req.hapticR, &req.vignette);

    // Optional diagnostics from the Lua side: "... #message"
    size_t hash = text.find('#');
    if (hash != std::string::npos) Log("Lua: %s", text.c_str() + hash + 1);

    std::string resp = g_handler ? g_handler(req) : std::string("TDVR2 0 0");

    if (g_respA) GlobalFree(g_respA);
    if (g_respW) GlobalFree(g_respW);
    g_respA = MakeGlobal(resp.c_str(), resp.size() + 1);
    std::wstring w(resp.begin(), resp.end());
    g_respW = MakeGlobal(w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    g_responseArmed = true;
    g_responseConsumed = false;

    if (++g_requestCount <= 3)
        Log("Bridge: Lua request #%d seq=%d flags=%d -> %zu bytes", g_requestCount, req.seq, req.flags, resp.size());
    return true;
}

BOOL WINAPI Hook_OpenClipboard(HWND hwnd)
{
    // Opening is deferred until we know whether this is a VR message.
    T('O');
    g_openHwnd = hwnd;
    g_realOpen = false;
    g_pendingEmpty = false;
    return TRUE;
}

BOOL WINAPI Hook_EmptyClipboard()
{
    T('E');
    g_pendingEmpty = true;
    return TRUE;
}

HANDLE WINAPI Hook_SetClipboardData(UINT fmt, HANDLE h)
{
    if (h && (fmt == CF_TEXT || fmt == CF_UNICODETEXT || fmt == CF_OEMTEXT)) {
        std::string text;
        if (void* p = GlobalLock(h)) {
            if (fmt == CF_UNICODETEXT) {
                const wchar_t* w = (const wchar_t*)p;
                for (size_t i = 0; w[i] && i < 64; ++i) text.push_back((char)w[i]);
                if (text.compare(0, 4, "TDVR") == 0) {
                    text.clear();
                    for (size_t i = 0; w[i]; ++i) text.push_back((char)w[i]);
                }
            } else {
                text = (const char*)p;
            }
            GlobalUnlock(h);
        }
        T('S', text.c_str());
        if (HandleRequest(text)) {
            g_pendingEmpty = false;
            GlobalFree(h);
            return h; // pretend success; caller no longer owns the handle
        }
    }
    if (!EnsureRealOpen()) return nullptr;
    return Real_SetClipboardData(fmt, h);
}

HANDLE WINAPI Hook_GetClipboardData(UINT fmt)
{
    T(g_responseArmed ? 'G' : 'g');
    if (g_responseArmed) {
        if (fmt == CF_UNICODETEXT) { g_responseConsumed = true; return g_respW; }
        if (fmt == CF_TEXT || fmt == CF_OEMTEXT) { g_responseConsumed = true; return g_respA; }
    }
    if (!EnsureRealOpen()) return nullptr;
    return Real_GetClipboardData(fmt);
}

BOOL WINAPI Hook_CloseClipboard()
{
    T('C');
    BOOL ok = TRUE;
    if (g_pendingEmpty) EnsureRealOpen(); // a real "clear clipboard" request
    if (g_realOpen) {
        ok = Real_CloseClipboard();
        g_realOpen = false;
    }
    if (g_responseConsumed) {
        g_responseArmed = false;
        g_responseConsumed = false;
    }
    return ok;
}

} // namespace

void BridgeDumpTrace()
{
    for (unsigned i = 0; i < 32; ++i) {
        const Trace& t = g_trace[(g_traceN + i) % 32];
        if (t.ev) Log("  clip[%2u] tid=%lu %c %s", i, t.tid, t.ev, t.text);
    }
}

void BridgeInit(LuaHandler handler)
{
    g_handler = handler;
    Real_OpenClipboard    = (PFN_OpenClipboard)HookIAT("USER32.dll", "OpenClipboard", (void*)Hook_OpenClipboard);
    Real_CloseClipboard   = (PFN_CloseClipboard)HookIAT("USER32.dll", "CloseClipboard", (void*)Hook_CloseClipboard);
    Real_EmptyClipboard   = (PFN_EmptyClipboard)HookIAT("USER32.dll", "EmptyClipboard", (void*)Hook_EmptyClipboard);
    Real_GetClipboardData = (PFN_GetClipboardData)HookIAT("USER32.dll", "GetClipboardData", (void*)Hook_GetClipboardData);
    Real_SetClipboardData = (PFN_SetClipboardData)HookIAT("USER32.dll", "SetClipboardData", (void*)Hook_SetClipboardData);

    // Fall back to the real functions if a hook could not be installed, so we
    // never leave a null function pointer behind.
    if (!Real_OpenClipboard)    Real_OpenClipboard = OpenClipboard;
    if (!Real_CloseClipboard)   Real_CloseClipboard = CloseClipboard;
    if (!Real_EmptyClipboard)   Real_EmptyClipboard = EmptyClipboard;
    if (!Real_GetClipboardData) Real_GetClipboardData = GetClipboardData;
    if (!Real_SetClipboardData) Real_SetClipboardData = SetClipboardData;
}
