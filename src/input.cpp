#include "input.h"
#include "config.h"
#include "engine.h"
#include "hooks.h"
#include "inventory.h"
#include "log.h"
#include "pckeys.h"
#include "ui.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Input {
namespace {

HWND g_hwnd = nullptr;
WNDPROC g_gameProc = nullptr;

using PFN_GetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using PFN_RegisterRawInputDevices = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
using PFN_SetCursorPos = BOOL(WINAPI*)(int, int);
using PFN_DefWindowProc = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using PFN_GetForegroundWindow = HWND(WINAPI*)();
PFN_GetRawInputData Real_GetRawInputData = GetRawInputData;
PFN_RegisterRawInputDevices Real_RegisterRawInputDevices = RegisterRawInputDevices;
PFN_SetCursorPos Real_SetCursorPos = SetCursorPos;
PFN_DefWindowProc Real_DefWindowProcA = DefWindowProcA;
PFN_DefWindowProc Real_DefWindowProcW = DefWindowProcW;
PFN_GetForegroundWindow Real_GetForegroundWindow = GetForegroundWindow;

// Focus while VR runs. The game's window procedure (+4f7f60) keeps an "active" flag from
// WM_ACTIVATE only (window+0x18 = LOWORD(wParam) != WA_INACTIVE && HIWORD(wParam) == 0); while
// it is 0, every frame clears its key / mouse-button "down" arrays (+4f1fd0). Its buttons come
// from the legacy WM_xBUTTONxxx messages this mod posts once per press: with the window
// inactive a held button lasted one frame (the right grip's grab, held fire, the extinguisher
// stopped at once - measured on the mock headset, with Notepad in front). In VR the player is
// in the headset and the desktop's window in front is anything (Virtual Desktop, a
// notification, another program). So while VR runs the game is kept active: every WM_ACTIVATE
// it gets says "active" and it never gets WM_ACTIVATEAPP / WM_KILLFOCUS deactivations
// (WndProc); it is told once that it is active if it was not (UpdateGameFocus). The real
// keyboard and mouse still go only to the window really in front (raw mouse without
// RIDEV_INPUTSINK, legacy keyboard), and the game, which centres the cursor on each raw mouse
// event while active, cannot move the real cursor then (Hook_SetCursorPos). VR stopped: the
// game is told the truth again. (It asks GetForegroundWindow only to bring itself to front or
// to open a link: not hooked.)
std::atomic<bool> g_vrFocus{ false };
bool g_gameActive = false;        // the game's own view (the activation messages it was given)
bool g_focusLossHidden = false;   // a deactivation was just kept from the game (Update releases Alt...)
ULONGLONG g_activationSent = 0;   // when UpdateGameFocus last posted an (de)activation

// --- Fake raw input --------------------------------------------------------
// Injected events are posted as WM_INPUT with a handle from this range; our
// GetRawInputData hook serves the data, so nothing leaks to other programs.
constexpr ULONG_PTR kFakeBase = 0x7EAD0000;
constexpr int kFakeSlots = 256;
std::mutex g_fakeMutex;
RAWINPUT g_fake[kFakeSlots];
int g_fakeNext = 0;
bool g_keyboardRaw = false, g_mouseRaw = false, g_noLegacy = false;

bool IsFake(HRAWINPUT h) { return (ULONG_PTR)h >= kFakeBase && (ULONG_PTR)h < kFakeBase + kFakeSlots; }

// Input messages that arrive while the next frame is a frozen (right-eye)
// frame are held back and re-posted before the next simulated frame.
struct DeferredMsg { UINT m; WPARAM w; LPARAM l; };
std::vector<DeferredMsg> g_deferred;

// Keys and clicks of the keys panel / quick key (SendPcKey), recognised by the
// window procedure so they reach the game while the VR panel is open (the
// desktop's real input and the controllers' usual keys are still held back then).
// Same thread as the window procedure (the game's main thread).
struct Injected { UINT m; WPARAM w; LPARAM l; ULONGLONG t; };
std::deque<Injected> g_injected;
bool g_notePcKeys = false;   // set while SendPcKey posts its messages

void NoteInjected(UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) return;   // (our raw input is recognised by its handle)
    ULONGLONG now = GetTickCount64();
    while (!g_injected.empty() && (g_injected.size() >= 64 || now - g_injected.front().t > 2000)) g_injected.pop_front();
    g_injected.push_back({ m, w, l, now });
}

bool TakeInjected(UINT m, WPARAM w, LPARAM l)
{
    for (auto it = g_injected.begin(); it != g_injected.end(); ++it)
        if (it->m == m && it->w == w && it->l == l) {
            g_injected.erase(it);
            return true;
        }
    return false;
}

// Posts an injected input message. While messages are held back, a new one is
// queued behind them instead: posted directly it would overtake them (a key-up
// arriving before the held key-down leaves the key stuck down in the game).
void PostInput(UINT m, WPARAM w, LPARAM l)
{
    if (g_notePcKeys) NoteInjected(m, w, l);
    if (!g_deferred.empty()) g_deferred.push_back({ m, w, l });
    else PostMessageW(g_hwnd, m, w, l);
}

void PostRaw(const RAWINPUT& ri)
{
    int slot;
    {
        std::lock_guard<std::mutex> lock(g_fakeMutex);
        slot = g_fakeNext;
        g_fakeNext = (g_fakeNext + 1) % kFakeSlots;
        g_fake[slot] = ri;
    }
    PostInput(WM_INPUT, RIM_INPUT, (LPARAM)(kFakeBase + slot));
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize)
{
    if (!IsFake(h)) return Real_GetRawInputData(h, cmd, data, size, headerSize);
    RAWINPUT ri;
    {
        std::lock_guard<std::mutex> lock(g_fakeMutex);
        ri = g_fake[(ULONG_PTR)h - kFakeBase];
    }
    UINT need = cmd == RID_HEADER ? sizeof(RAWINPUTHEADER) : ri.header.dwSize;
    if (!data) { *size = need; return 0; }
    if (*size < need) { *size = need; return (UINT)-1; }
    memcpy(data, &ri, need);
    return need;
}

BOOL WINAPI Hook_RegisterRawInputDevices(PCRAWINPUTDEVICE devs, UINT n, UINT cb)
{
    for (UINT i = 0; i < n; ++i) {
        Log("Game RegisterRawInputDevices: page=%u usage=%u flags=0x%x hwnd=%p", devs[i].usUsagePage, devs[i].usUsage,
            devs[i].dwFlags, devs[i].hwndTarget);
        if (devs[i].usUsagePage == 1 && devs[i].usUsage == 6) g_keyboardRaw = !(devs[i].dwFlags & RIDEV_REMOVE);
        if (devs[i].usUsagePage == 1 && devs[i].usUsage == 2) g_mouseRaw = !(devs[i].dwFlags & RIDEV_REMOVE);
        if (devs[i].dwFlags & RIDEV_NOLEGACY) g_noLegacy = true;
    }
    return Real_RegisterRawInputDevices(devs, n, cb);
}

LRESULT WINAPI Hook_DefWindowProcA(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT && IsFake((HRAWINPUT)l)) return 0;
    return Real_DefWindowProcA(h, m, w, l);
}
LRESULT WINAPI Hook_DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT && IsFake((HRAWINPUT)l)) return 0;
    return Real_DefWindowProcW(h, m, w, l);
}

bool g_blockCursor = false;
POINT g_lastPointer = { -1, -1 };   // last VR pointer position sent to the game (client pixels)
Mode g_lastMode = Mode::Off;
// A stick used as the mouse (see StickMouse).
int g_mouseStick = 0;               // 0 = none, 1 = movement stick, 2 = rotation stick
bool g_mouseAlways = false;         // forced on from the keys panel
float g_mouseRest[2] = {};          // fractions of a mouse count not sent yet
POINT g_virtCursor = { 0, 0 };      // its cursor, for the mods' own windows (client pixels)
bool g_virtValid = false;
BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    if (g_blockCursor) return TRUE;
    // (kept active while another window is in front - see g_vrFocus: it must not move the
    // real cursor of the program really in front)
    if (g_vrFocus && g_hwnd && Real_GetForegroundWindow() != g_hwnd) return TRUE;
    return Real_SetCursorPos(x, y);
}

// --- Injection primitives ----------------------------------------------------
float g_screenU = -1, g_screenV = -1;

// Where a mouse message should land (client pixels): the VR pointer while it
// points at the virtual screen, the Windows cursor otherwise.
POINT PointerClientPos()
{
    if (g_mouseStick && g_virtValid) return g_virtCursor;   // the stick is the mouse: its cursor
    POINT p = g_lastPointer;
    if (g_screenU < 0 || p.x < 0) {
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
    }
    return p;
}

void SendKey(WORD vk, bool down)
{
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    bool ext = vk == VK_RCONTROL || vk == VK_INSERT || vk == VK_DELETE || vk == VK_LEFT || vk == VK_RIGHT ||
               vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT;
    if (g_keyboardRaw) {
        RAWINPUT ri = {};
        ri.header.dwType = RIM_TYPEKEYBOARD;
        ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
        ri.header.hDevice = nullptr;
        ri.header.wParam = RIM_INPUT;
        ri.data.keyboard.MakeCode = (USHORT)scan;
        ri.data.keyboard.Flags = (down ? RI_KEY_MAKE : RI_KEY_BREAK) | (ext ? RI_KEY_E0 : 0);
        ri.data.keyboard.VKey = vk;
        ri.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
        PostRaw(ri);
    }
    if (!g_keyboardRaw || !g_noLegacy) {
        LPARAM lp = 1 | (scan << 16) | (ext ? (1 << 24) : 0) | (down ? 0 : (3u << 30));
        PostInput(down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    }
}

// A key given by its position on a US keyboard (scan code) AND its US letter: the
// game binds movement to "w a s d"; on an AZERTY keyboard the letter W is not at
// the W position, so both readings (virtual key or scan code) must point to it.
void SendKeyPos(WORD vk, UINT scan, bool down)
{
    if (g_keyboardRaw) {
        RAWINPUT ri = {};
        ri.header.dwType = RIM_TYPEKEYBOARD;
        ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
        ri.header.wParam = RIM_INPUT;
        ri.data.keyboard.MakeCode = (USHORT)scan;
        ri.data.keyboard.Flags = down ? RI_KEY_MAKE : RI_KEY_BREAK;
        ri.data.keyboard.VKey = vk;
        ri.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
        PostRaw(ri);
    }
    if (!g_keyboardRaw || !g_noLegacy) {
        LPARAM lp = 1 | (scan << 16) | (down ? 0 : (3u << 30));
        PostInput(down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    }
}

void SendMouse(USHORT buttonFlags, short wheel, LONG dx, LONG dy)
{
    if (g_mouseRaw) {
        RAWINPUT ri = {};
        ri.header.dwType = RIM_TYPEMOUSE;
        ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
        ri.header.wParam = RIM_INPUT;
        ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
        ri.data.mouse.usButtonFlags = buttonFlags;
        ri.data.mouse.usButtonData = (USHORT)wheel;
        ri.data.mouse.lLastX = dx;
        ri.data.mouse.lLastY = dy;
        PostRaw(ri);
    }
}

// Dual wield (DualClicks): the legacy left-button messages of our click carry this position (set
// before they are posted: the window procedure may see them at once on another thread).
std::atomic<LPARAM> g_lbuttonL{ 0 };

void SendMouseButton(int button, bool down)
{
    static const USHORT downFlags[3] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN };
    static const USHORT upFlags[3] = { RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP };
    SendMouse(down ? downFlags[button] : upFlags[button], 0, 0, 0);
    if (!g_mouseRaw || !g_noLegacy) {
        static const UINT downMsg[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
        static const UINT upMsg[3] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
        POINT p = PointerClientPos();
        if (button == 0) g_lbuttonL.store(MAKELPARAM(p.x, p.y));
        PostInput(down ? downMsg[button] : upMsg[button], 0, MAKELPARAM(p.x, p.y));
    }
}

void SendWheel(int notches)
{
    SendMouse(RI_MOUSE_WHEEL, (short)(notches * WHEEL_DELTA), 0, 0);
    if (!g_mouseRaw || !g_noLegacy) {
        POINT p = PointerClientPos();
        ClientToScreen(g_hwnd, &p);   // WM_MOUSEWHEEL uses screen coordinates
        PostInput(WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(notches * WHEEL_DELTA)), MAKELPARAM(p.x, p.y));
    }
}

// Any key of the PcKeys table. Keyboard keys: US virtual key and US scan code
// (see SendKeyPos). A wheel step is sent when "pressed" (nothing on release).
void SendPcKey(int key, bool down)
{
    struct Note { Note() { g_notePcKeys = true; } ~Note() { g_notePcKeys = false; } } note;   // (see TakeInjected)
    const PcKeys::Def& k = PcKeys::Get(key);
    if (k.kind == PcKeys::Mouse) {
        SendMouseButton(k.vk, down);
        return;
    }
    if (k.kind == PcKeys::Wheel) {
        if (down) SendWheel(k.vk ? 1 : -1);
        return;
    }
    const bool ext = k.kind == PcKeys::ExtKey;
    if (g_keyboardRaw) {
        RAWINPUT ri = {};
        ri.header.dwType = RIM_TYPEKEYBOARD;
        ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
        ri.header.wParam = RIM_INPUT;
        ri.data.keyboard.MakeCode = k.scan;
        ri.data.keyboard.Flags = (down ? RI_KEY_MAKE : RI_KEY_BREAK) | (ext ? RI_KEY_E0 : 0);
        ri.data.keyboard.VKey = k.vk;
        ri.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
        PostRaw(ri);
    }
    if (!g_keyboardRaw || !g_noLegacy) {
        LPARAM lp = 1 | ((LPARAM)k.scan << 16) | (ext ? (1 << 24) : 0) | (down ? 0 : (3u << 30));
        PostInput(down ? WM_KEYDOWN : WM_KEYUP, k.vk, lp);
    }
}

// A stick as the mouse: raw mouse movement (the game's camerax / cameray and
// mousedx / mousedy) plus a cursor for the windows mods draw with
// UiMakeInteractive (they follow WM_MOUSEMOVE, like the game's menus).
constexpr float kMouseCountsPerSecond = 1500;   // stick fully pushed, speed x1
void StickMouse(float x, float y, float dt)
{
    // Dead zone, then a curve: small pushes for precise aiming, full push to turn fast.
    auto curve = [](float v) {
        float a = std::abs(v);
        if (a < 0.12f) return 0.0f;
        a = std::min(1.0f, (a - 0.12f) / 0.88f);
        return (v < 0 ? -a : a) * a;
    };
    float speed = kMouseCountsPerSecond * std::clamp(Cfg().modMouseSpeed, 0.1f, 5.0f);
    dt = std::min(dt, 0.1f);
    g_mouseRest[0] += curve(x) * speed * dt;
    g_mouseRest[1] -= curve(y) * speed * dt;   // stick up = mouse up (screen y goes down)
    LONG dx = (LONG)g_mouseRest[0], dy = (LONG)g_mouseRest[1];
    g_mouseRest[0] -= (float)dx;
    g_mouseRest[1] -= (float)dy;
    RECT rc = {};
    if (g_hwnd) GetClientRect(g_hwnd, &rc);
    if (!g_virtValid) {
        g_virtCursor = { rc.right / 2, rc.bottom / 2 };
        g_virtValid = true;
    }
    if (dx == 0 && dy == 0) return;
    SendMouse(0, 0, dx, dy);
    if (g_hwnd && rc.right > 0 && rc.bottom > 0) {
        g_virtCursor.x = std::clamp<LONG>(g_virtCursor.x + dx, 0, rc.right - 1);
        g_virtCursor.y = std::clamp<LONG>(g_virtCursor.y + dy, 0, rc.bottom - 1);
        PostMessageW(g_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(g_virtCursor.x, g_virtCursor.y));
    }
}

// --- Virtual "buttons" driven by the controllers ----------------------------
enum Act { A_UseTool, A_Grab, A_Interact, A_Crouch, A_Jump, A_Flashlight, A_Map, A_Pause, A_MenuClick, A_MenuBack,
           A_Forward, A_Backward, A_Left, A_Right, A_Count };
bool g_actDown[A_Count] = {};

void SetAct(Act a, bool down)
{
    if (g_actDown[a] == down) return;
    g_actDown[a] = down;
    static int logged = 0;
    if (logged < 40) {
        ++logged;
        static const char* names[] = { "UseTool", "Grab", "Interact", "Crouch", "Jump", "Flashlight", "Map", "Pause",
                                       "MenuClick", "MenuBack", "Forward", "Backward", "Left", "Right" };
        Log("Input: %s %s (keyboard raw=%d mouse raw=%d)", names[a], down ? "down" : "up", g_keyboardRaw, g_mouseRaw);
    }
    switch (a) {
    case A_UseTool:   SendMouseButton(0, down); break;
    case A_Grab:      SendMouseButton(1, down); break;
    case A_Interact:  SendKey('E', down); break;
    case A_Crouch:    SendKey(VK_CONTROL, down); break;
    case A_Jump:      SendKey(VK_SPACE, down); break;
    case A_Flashlight:SendKey('F', down); break;
    case A_Map:       SendKey(VK_TAB, down); break;
    case A_Pause:     SendKey(VK_ESCAPE, down); break;
    case A_MenuClick: SendMouseButton(0, down); break;
    case A_MenuBack:  SendKey(VK_ESCAPE, down); break;
    case A_Forward:   SendKeyPos('W', 0x11, down); break;
    case A_Backward:  SendKeyPos('S', 0x1F, down); break;
    case A_Left:      SendKeyPos('A', 0x1E, down); break;
    case A_Right:     SendKeyPos('D', 0x20, down); break;
    default: break;
    }
}

void ReleaseAll()
{
    for (int i = 0; i < A_Count; ++i) SetAct((Act)i, false);
}

// --- Dual wield: our click reaching the game (see DualClicks) ------------------------------------
// The window procedure (the game's main thread) tells when the down / up of our click is handed to
// the game: its legacy message with the position we posted it with (the game takes its buttons from
// those), or - when the game gets raw input only - our fake WM_INPUT carrying that button change.
std::atomic<int> g_dualExpect{ 0 };          // 0 = nothing, 1 = our click's down, 2 = its up
std::atomic<bool> g_dualLegacy{ true };       // our click has legacy messages (else raw input only)
std::atomic<int> g_dualSeen{ 0 };             // the last one seen (1 down, 2 up)
std::atomic<unsigned long long> g_dualSeenFrame{ 0 };   // game updates so far when it was seen

void DualWatch(UINT m, LPARAM l)
{
    const int e = g_dualExpect.load();
    if (e == 0) return;
    bool ours = false;
    if (g_dualLegacy.load()) {
        ours = m == (e == 1 ? (UINT)WM_LBUTTONDOWN : (UINT)WM_LBUTTONUP) && l == g_lbuttonL.load();
    } else if (m == WM_INPUT && IsFake((HRAWINPUT)l)) {
        RAWINPUT ri;
        {
            std::lock_guard<std::mutex> lock(g_fakeMutex);
            ri = g_fake[(ULONG_PTR)l - kFakeBase];
        }
        ours = ri.header.dwType == RIM_TYPEMOUSE &&
               (ri.data.mouse.usButtonFlags & (e == 1 ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_UP)) != 0;
    }
    if (!ours) return;
    g_dualExpect = 0;
    g_dualSeenFrame = EngineFrameCounter();
    g_dualSeen = e;
    if (e == 1) EngineDualClickDelivered();
}

// --- Window procedure ---------------------------------------------------------
std::unordered_map<UINT, int> g_msgCounts;

bool IsDeferrable(UINT m, LPARAM l)
{
    switch (m) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
        return true;
    case WM_INPUT:
        return IsFake((HRAWINPUT)l);
    default:
        return false;
    }
}
float g_wheelAccum = 0;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    // Focus (see g_vrFocus): while VR runs the game never hears that its window was deactivated.
    // (the game's own rule: active = not WA_INACTIVE and not minimised)
    const bool gameAct = LOWORD(w) != WA_INACTIVE && HIWORD(w) == 0;
    if (m == WM_ACTIVATE && g_vrFocus && !gameAct) {
        w = MAKEWPARAM(WA_ACTIVE, 0);   // (an inactive or minimised activation: told active)
        g_focusLossHidden = true;
        static int logs = 0;
        if (logs < 20) {
            ++logs;
            Log("Input: the game window was deactivated - the game is told it stays active while VR runs (held buttons keep working)");
        }
    }
    const bool deact = (m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && !(LOWORD(w) != WA_INACTIVE && HIWORD(w) == 0)) ||
                       m == WM_KILLFOCUS;
    const bool act = (m == WM_ACTIVATEAPP && w) || (m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE && HIWORD(w) == 0) ||
                     m == WM_SETFOCUS;
    if (deact && g_vrFocus) {
        g_focusLossHidden = true;
        static int logs = 0;
        if (logs < 20) {
            ++logs;
            Log("Input: the game window lost the focus (msg 0x%04x) - not passed to the game while VR runs (held buttons keep working)", m);
        }
        return 0;
    }
    if (m == WM_NCACTIVATE && !w && g_vrFocus) return Real_DefWindowProcW(h, m, w, l);   // (title bar only)
    if (deact) g_gameActive = false;
    if (act) g_gameActive = true;
    int& c = g_msgCounts[m];
    if (c++ == 0 && (m == WM_INPUT || m == WM_KEYDOWN || m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN || m == WM_CHAR ||
                     m == WM_MOUSEWHEEL || m == WM_SYSKEYDOWN))
        Log("Game window receives msg 0x%04x for the first time", m);

    // F10 arrives as WM_SYSKEYDOWN (it is the Windows "menu bar" key).
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1 << 30))) {
        if (w == VK_F10) { Ui::Toggle(); return 0; }
        if (w == VK_F8) { VrRequestRecenter(); return 0; }
    }
    // While the VR pointer is on the virtual screen, every mouse message carries
    // its position (the real cursor - clamped to the desktop, or moved by Virtual
    // Desktop - would otherwise alternate with it and menu items would flicker).
    if (g_lastMode == Mode::Screen && g_screenU >= 0 && g_lastPointer.x >= 0 &&
        (m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_RBUTTONDOWN || m == WM_RBUTTONUP ||
         m == WM_MBUTTONDOWN || m == WM_MBUTTONUP))
        l = MAKELPARAM(g_lastPointer.x, g_lastPointer.y);
    if (EngineNextFrameFrozen() && IsDeferrable(m, l)) {
        g_deferred.push_back({ m, w, l });
        return 0;
    }
    // (keys and clicks this mod sends for the player still reach the game: keys panel)
    if (Ui::Visible() && Cfg().showDesktopUi && !TakeInjected(m, w, l)) {
        // The settings window owns the mouse & keyboard while it is open.
        switch (m) {
        case WM_INPUT:
            if (!IsFake((HRAWINPUT)l)) return Real_DefWindowProcW(h, m, w, l);
            break;
        case WM_MOUSEWHEEL:
            g_wheelAccum += GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA;
            return 0;
        // (WM_KEYUP still reaches the game: a key it saw pressed - throttle W in a
        // vehicle, crouch, E... - must not stay down while the panel is open)
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_KEYDOWN: case WM_CHAR:
            return 0;
        default:
            break;
        }
    }
    DualWatch(m, l);   // (dual wield: our click reaching the game)
    return CallWindowProcW(g_gameProc, h, m, w, l);
}

// --- Controller state tracking -----------------------------------------------
float g_menuHeld = 0;
float g_recenterHeld = 0;
bool g_menuWasDown = false;
bool g_menuLongFired = false;
float g_pulse[A_Count] = {};
// Press a key briefly. It must stay down for several game frames (and not be
// released in the same message batch) for the game to see it.
void Pulse(Act a) { g_pulse[a] = 0.12f; }
float g_scrollRepeat = 0;
int g_lastScrollDir = 0;
bool g_mapLatch = false;
// A trigger / grip held while getting in or out of a vehicle counts as released
// until let go: otherwise the left trigger that got in (E) reverses the vehicle
// (S) and the throttle held while getting out fires the tool.
bool g_analogLatch[4] = {};   // dominant trigger, off-hand trigger, dominant grip, off-hand grip
bool g_latchVehicle = false;
// The off hand's grip on foot: (Bonelabs-like physical off hand, main.lua tdvrOffHandTick)
// grab the object that hand touches, or pull into the hand the object its laser points at
// (tdvrOffLaserTick, 3 m) - always a grab press, like the dominant hand's grip; nothing
// there, nothing happens (the player lets go and grips again). Crouching is on the off
// hand's trigger. Without offArm, offHandPhysics and offGrab the grip crouches too.
int g_offGripState = 0;          // 0 = up, 1 = crouch press, 2 = grab press
int g_offTrigState = 0;          // the off hand's trigger (L2) on foot: 0 = up, 1 = crouch
int g_offGripShown = 0;          // the same, but 0 outside gameplay on foot (OffGripPress)
std::atomic<bool> g_offReach{ false };          // "TDVRH" (SetOffHandReach, Lua thread)
std::atomic<bool> g_offNear{ false };           // ... of it, an object right under the hand
std::atomic<ULONGLONG> g_offReachTime{ 0 };
int g_offGrabLogs = 0;

bool Hyst(bool prev, float v, float on = 0.55f, float off = 0.35f) { return prev ? v > off : v > on; }

// Focus (see g_vrFocus): VR running and the game thinking its window inactive (it was not in
// front when VR started, or the player clicked another program before): it is told it is
// active; VR stopped while its window is not really in front: it is told the truth. (Posted:
// handled by the window procedure like the real messages; once per second at most.)
void UpdateGameFocus()
{
    if (!g_hwnd || !g_gameProc) return;
    const bool front = Real_GetForegroundWindow() == g_hwnd;
    const ULONGLONG now = GetTickCount64();
    if (now - g_activationSent < 1000) return;
    if (g_vrFocus && !g_gameActive) {
        g_activationSent = now;
        Log("Input: VR runs and the game thought its window inactive (%s) - told it is active",
            front ? "it is in front" : "another window is in front");
        PostMessageW(g_hwnd, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageW(g_hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessageW(g_hwnd, WM_SETFOCUS, 0, 0);
    } else if (!g_vrFocus && g_gameActive && !front) {
        g_activationSent = now;
        Log("Input: VR stopped while another window is in front - the game is told its window is inactive");
        PostMessageW(g_hwnd, WM_KILLFOCUS, 0, 0);
        PostMessageW(g_hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        PostMessageW(g_hwnd, WM_ACTIVATEAPP, FALSE, 0);
    }
}

// --- PC keys pressed for the player (keys panel, quick key) ------------------------
struct Tap { int key; float left; };
std::vector<Tap> g_taps;           // keys pressed briefly: released when "left" runs out
int g_holdRequest = -1;            // key the keys panel holds this frame
std::vector<int> g_latched;        // modifiers kept down while the keys panel is open
std::vector<bool> g_pcDown;        // keys this mod holds down in the game
float g_wheelRepeat = 0;           // a held wheel button repeats its step
bool g_clickMoved = false;         // the movement stick was pushed during its click
float g_scriptCamHold = 0;         // seconds the script camera still counts (see Update)

// The headset shows a mod's camera away from the player (plane, drone... views): the same
// test as the engine's view (a mod camera at the player's eyes - scope, camera mod - or
// modCameraVr off keeps the normal head view, and the VR inventory with it).
bool ModCameraView() { return g_scriptCamHold > 0 && Cfg().modCameraVr && !Inventory::CameraNearPlayer(); }

// Presses / releases what the taps, the panel and the latches want this frame.
void UpdatePcKeys(float dt, bool keysPanelOpen)
{
    const int n = PcKeys::Count();
    if ((int)g_pcDown.size() != n) g_pcDown.assign(n, false);
    if (!keysPanelOpen) {
        g_holdRequest = -1;
        g_latched.clear();
    }
    std::vector<bool> want(n, false);
    for (auto it = g_taps.begin(); it != g_taps.end();) {
        want[it->key] = true;
        it->left -= dt;
        it = it->left <= 0 ? g_taps.erase(it) : it + 1;
    }
    if (g_holdRequest >= 0 && g_holdRequest < n) want[g_holdRequest] = true;
    for (int k : g_latched) want[k] = true;
    for (int k = 0; k < n; ++k) {
        bool wheel = PcKeys::Get(k).kind == PcKeys::Wheel;
        if (want[k] != g_pcDown[k]) {
            g_pcDown[k] = want[k];
            SendPcKey(k, want[k]);
            if (want[k] && wheel) g_wheelRepeat = 0.4f;
        } else if (want[k] && wheel && k == g_holdRequest && (g_wheelRepeat -= dt) <= 0) {
            SendPcKey(k, true);   // (wheel button held: one more step)
            g_wheelRepeat = 0.12f;
        }
    }
    g_holdRequest = -1;   // (the panel asks again next frame)
}

} // namespace

void Init(HWND hwnd)
{
    g_hwnd = hwnd;
    g_gameActive = Real_GetForegroundWindow() == hwnd;   // (then followed by its activation messages)
    g_gameProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
    Log("Input: window procedure subclassed (game proc %p)", g_gameProc);
}

HWND GameWindow() { return g_hwnd; }

void FlushDeferred()
{
    // A stereo pair whose frozen frame never comes (no game update between
    // presents) must not hold the input back for ever.
    static int pendingPresents = 0;
    if (EngineNextFrameFrozen()) {
        if (++pendingPresents < 3) return;
        EngineCancelStereo();
    }
    pendingPresents = 0;
    if (g_deferred.empty()) return;
    for (const DeferredMsg& d : g_deferred) PostMessageW(g_hwnd, d.m, d.w, d.l);
    g_deferred.clear();
}

float g_swingTime = 0;
void RequestSwing() { g_swingTime = 0.12f; }

// The game's sledge on the dominant trigger, as the player asked: a light, short pull is one
// whole strike, at the game's reach, the trigger held or not. The game starts its swing when
// "use tool" is PRESSED (Sledge::update 0x450690: action 9's "pressed" byte, input +0xdad, once
// its 0.3 s cooldown +0x138 is over) and only looks for what it hits 0.26 s later (its timer
// +0x130 going from 0.01 past 0.47 at 1.8 /s): a 0.3 m sphere cast from the player's camera
// (+0x640, turned toward the laser target by main.lua) up to 3 m - the button no longer matters
// then. So with the sledge in hand the trigger presses from a light pull (sledgeTrigger, released
// at half of it; 55 % / 35 % for the other tools) and each press lasts at least kSledgeHold, so a
// flick still reaches the game in its own frames (see Pulse). A new pull while that short press
// is still down: the button is released for kSledgeGap, then pressed again (otherwise the game
// would see no new press). (main.lua keeps the physical copy of the tool out of the strike's way
// until the strike is resolved - updateTool, phys.strikeT: with the trigger let go before 0.26 s,
// the copy, which follows the swing in front of the camera, got its collisions back and the
// strike hit the copy instead of the object.) A pull that was not the game's is no strike until
// the trigger is let go (g_sledgeLatch): with the old 55 % threshold, the ends of those pulls were
// below it already.
static constexpr float kSledgeHold = 0.15f;   // s: shortest press given to the game
static constexpr float kSledgeGap = 0.06f;    // s: release between two presses
static bool g_sledgeTrig = false;             // the trigger, with the sledge's light thresholds
static bool g_sledgeLatch = false;            // a pull that was not the game's: no strike until let go
static float g_sledgeHold = 0;                // press time still due (s)
static float g_sledgeGap = 0;                 // release still due before the next press (s)
static bool g_sledgeAgain = false;            // a press waits for the end of that release
static unsigned long long g_sledgeCall = 0;   // engine frame of the last call
static int g_sledgeLogs = 0;

// "Use tool" from the dominant trigger while the sledge is in hand (see above). sledge false:
// the state only follows the button (a tool changed to the sledge with the trigger already
// held is no new press, as before). captured: the VR inventory takes the trigger.
static bool SledgeUse(bool sledge, float trigger, bool captured, float dt)
{
    // (not called for a while - a vehicle, a menu, our settings panel, dual wield...: nothing of an
    // older press is kept)
    const unsigned long long frame = EngineFrameCounter();
    const bool stale = frame - g_sledgeCall > 8;
    g_sledgeCall = frame;
    const float on = std::clamp(Cfg().sledgeTrigger, 0.05f, 0.95f), off = on * 0.5f;
    // (a pull that was not the game's: one the VR inventory took - it lets the trigger go at 35 % -,
    // one still held when this was not called for a while - our settings panel closed with its
    // "Fermer" button: ImGui clicks on release, below 60 % -, one begun with another tool or while the
    // game's grab held an object: counts as released until the trigger drops below the release
    // threshold, so its end is no strike)
    if (captured || (trigger > off && (stale || !sledge))) g_sledgeLatch = true;
    else if (trigger <= off) g_sledgeLatch = false;
    if (g_sledgeLatch) trigger = 0;
    if (!sledge || stale) {
        g_sledgeTrig = g_actDown[A_UseTool];
        g_sledgeHold = g_sledgeGap = 0;
        g_sledgeAgain = false;
        if (!sledge) return false;
    }
    dt = std::min(dt, 0.05f);   // (one long frame - a hitch - does not end a short press by itself)
    const bool was = g_sledgeTrig;
    g_sledgeTrig = Hyst(was, trigger, on, off);
    bool press = g_sledgeTrig && !was;
    g_sledgeHold = std::max(0.0f, g_sledgeHold - dt);
    if (g_sledgeGap > 0) {
        g_sledgeAgain = g_sledgeAgain || press;
        g_sledgeGap = std::max(0.0f, g_sledgeGap - dt);
        if (g_sledgeGap > 0) return false;   // (still released)
        press = g_sledgeAgain;
        g_sledgeAgain = false;
    } else if (press && g_actDown[A_UseTool]) {
        // (the last short press is still down: released first)
        g_sledgeGap = kSledgeGap;
        g_sledgeAgain = true;
        if (g_sledgeLogs < 60) {
            ++g_sledgeLogs;
            Log("Input: sledge - new pull while the last short press is still down: released for %.2f s, then "
                "pressed again", kSledgeGap);
        }
        return false;
    }
    if (press) {
        g_sledgeHold = kSledgeHold;
        if (g_sledgeLogs < 60) {
            ++g_sledgeLogs;
            Log("Input: sledge strike - trigger %.2f (presses from %.2f), button down at least %.2f s; the game "
                "swings and hits 0.26 s later, up to 3 m, button held or not", trigger, on, kSledgeHold);
        }
    }
    return g_sledgeTrig || g_sledgeHold > 0;
}

bool g_inVehicle = false;
void SetVehicle(bool inVehicle) { g_inVehicle = inVehicle; }

// Keys are on/off: a stick half pushed presses the key half of the time
// (0.25 s cycle), so steering follows the stick progressively.
float g_pwmPhase[3] = {};
float g_keyRefresh = 0;
int g_keyRefreshLeft = 0;   // releases of W/A/S/D still to repeat after leaving a vehicle
bool Pwm(float v, float& phase, float dt)
{
    v = std::abs(v);
    if (v < 0.2f) { phase = 0; return false; }
    if (v > 0.85f) return true;
    const float period = 0.25f;
    phase += dt;
    if (phase >= period) phase -= period;
    return phase < (v - 0.2f) / 0.65f * period;
}

void SetScreenPointer(float u, float v)
{
    g_screenU = u;
    g_screenV = v;
}

void TapKey(int key)
{
    if (key < 0 || key >= PcKeys::Count()) return;
    for (Tap& t : g_taps)
        if (t.key == key) {
            t.left = 0.12f;
            return;
        }
    g_taps.push_back({ key, 0.12f });   // (like Pulse: long enough for the game to see it)
}

void HoldKey(int key) { g_holdRequest = key; }

void SetLatched(int key, bool down)
{
    auto it = std::find(g_latched.begin(), g_latched.end(), key);
    if (down && it == g_latched.end() && key >= 0 && key < PcKeys::Count()) g_latched.push_back(key);
    if (!down && it != g_latched.end()) g_latched.erase(it);
}

bool Latched(int key) { return std::find(g_latched.begin(), g_latched.end(), key) != g_latched.end(); }

void SetMouseAlways(bool on) { g_mouseAlways = on; }
bool MouseAlways() { return g_mouseAlways; }
int MouseStick() { return g_mouseStick; }

void MaskMouseStick(HandState& left, HandState& right)
{
    if (!g_mouseStick) return;
    // 1 = movement stick (off hand), 2 = rotation stick (dominant hand)
    bool leftIsMouse = (g_mouseStick == 1) != Cfg().leftHanded;
    HandState& h = leftIsMouse ? left : right;
    h.stickX = h.stickY = 0;
}

// Pointer hand (see input.h). -1 = the dominant hand. (Atomic: a click on a mod's window sets it
// too, from the engine's frame - engine.cpp, ModUiPointer.)
std::atomic<int> g_pointerHand{ -1 };
bool g_pointerReported[2] = {};   // ReportPointerHit since the last UpdatePointerHand
bool g_pointerTrig[2] = {};       // each trigger held (with hysteresis, like the panels)

int PointerHand()
{
    const int dom = Cfg().leftHanded ? 0 : 1, h = g_pointerHand;
    return Cfg().offHandPointer && h >= 0 ? h : dom;
}

bool TakePointer(int hand)
{
    if (hand != 0 && hand != 1) return false;
    if (!Cfg().offHandPointer) return true;   // (the window keeps its own hand: engine.cpp)
    const int cur = PointerHand();
    if (hand == cur) return true;
    static int logged = 0;   // (diagnostics, a few lines only)
    if (g_pointerTrig[cur]) {   // (a press of the pointer hand in progress keeps its hand: UpdatePointerHand)
        if (logged < 30) {
            ++logged;
            Log("Input: pointer hand stays %s - the %s trigger pulled on a mod's window while the pointer hand's "
                "trigger is held", cur ? "right" : "left", hand ? "right" : "left");
        }
        return false;
    }
    g_pointerHand = hand;
    if (logged < 30) {
        ++logged;
        Log("Input: pointer hand -> %s (its trigger pulled on a mod's window)", hand ? "right" : "left");
    }
    return true;
}

void ReportPointerHit(int hand)
{
    if (hand == 0 || hand == 1) g_pointerReported[hand] = true;
}

void UpdatePointerHand(bool hitLeft, bool hitRight, float triggerLeft, float triggerRight)
{
    const bool hit[2] = { hitLeft || g_pointerReported[0], hitRight || g_pointerReported[1] };
    g_pointerReported[0] = g_pointerReported[1] = false;
    const float trig[2] = { triggerLeft, triggerRight };
    bool pressed[2];
    for (int i = 0; i < 2; ++i) {
        const bool down = g_pointerTrig[i] ? trig[i] > 0.35f : trig[i] > 0.55f;
        pressed[i] = down && !g_pointerTrig[i];
        g_pointerTrig[i] = down;
    }
    if (!Cfg().offHandPointer) {
        g_pointerHand = -1;
        return;
    }
    // The other hand takes the pointer only by pulling its trigger with its ray on one of our
    // interfaces: that press is its first click there, at its own ray (the interfaces read
    // PointerHand() after this, in the same present). Its ray alone on one - the pointer hand's ray
    // beside it, or crossing where the other one points - changes nothing: the pointer stays with
    // the hand that clicked last (as the player asked: before, the hand alone on an interface took
    // it, and the hands swapped the pointer by themselves). A press in progress keeps its hand; both
    // triggers pulled in the same present: the pointer hand's, unless only the other one's ray is on
    // an interface.
    const int cur = PointerHand(), other = 1 - cur;
    const bool busy = g_pointerTrig[cur] && !pressed[cur];
    if (!busy && pressed[other] && hit[other] && !(pressed[cur] && hit[cur])) g_pointerHand = other;
    static int logged = 0, kept = 0;   // (diagnostics: which hand drives the panels; a few lines only)
    static bool lone = false;          // only the other hand's ray was on an interface (last present)
    const bool nowLone = hit[other] && !hit[cur];
    if (PointerHand() != cur) {
        if (logged < 30) {
            ++logged;
            Log("Input: pointer hand -> %s (its trigger pulled on a panel)", PointerHand() ? "right" : "left");
        }
    } else if (nowLone && !lone && kept < 30) {
        ++kept;
        Log("Input: pointer hand stays %s - only the %s hand's ray is on a panel (it takes the pointer when its "
            "trigger is pulled there)", cur ? "right" : "left", other ? "right" : "left");
    }
    lone = nowLone && PointerHand() == cur;
}

std::atomic<bool> g_releaseMoveKeys{ false };

void RequestReleaseMovementKeys() { g_releaseMoveKeys = true; }

void SetOffHandReach(bool reach, bool underHand)
{
    g_offReach = reach;
    g_offNear = reach && underHand;
    g_offReachTime = GetTickCount64();
}

bool OffHandTouching() { return g_offReach && GetTickCount64() - g_offReachTime < 1500; }

bool OffHandNear() { return g_offNear && GetTickCount64() - g_offReachTime < 1500; }

int OffGripPress() { return g_offGripShown; }

std::atomic<bool> g_playerGrabbing{ false };
void SetPlayerGrabbing(bool grabbing) { g_playerGrabbing = grabbing; }

// grabKeepDistance (engine.cpp GrabControls, main.lua tdvrOffHandTick): while a hand holds an object at a
// distance - the game's grab (dominant grip, Lua flag kLuaFlagGrabbing) or the off hand's far grab (Lua flag
// kLuaFlagOffFarHold) - the dominant stick's up / down moves it farther / closer, smoothly (m/s, + = farther,
// 0.2 dead zone), instead of the mouse wheel's 25 cm steps, and never changes the tool. Both hands holding:
// the hand that grabbed last. Nothing while that stick is the mouse (a mod's camera) or is pushed more
// sideways than up / down (turning is unchanged). Setting off, or the game's grab code not recognised for
// the dominant hand: the wheel as before.
namespace {
std::atomic<bool> g_offFarHold{ false };
std::atomic<float> g_offGrabVel{ 0.0f };
int g_grabStickHand = 0;   // 1 = the game's grab, 2 = the off hand's far grab, 0 = none

// True: no wheel step this frame - a held object takes this stick's up / down, or the stick is still
// pushed since it did (the grip let go while pushing it away must not change the tool next).
bool GrabStick(const HandState& dom)
{
    static bool prevGame = false, prevOff = false, latch = false;
    const bool game = g_playerGrabbing, off = g_offFarHold;
    if (game && !prevGame) g_grabStickHand = 1;
    if (off && !prevOff) g_grabStickHand = 2;
    if (g_grabStickHand == 1 && !game) g_grabStickHand = off ? 2 : 0;
    if (g_grabStickHand == 2 && !off) g_grabStickHand = game ? 1 : 0;
    prevGame = game;
    prevOff = off;
    const Settings& c = Cfg();
    const bool on = c.grabKeepDistance && g_grabStickHand != 0 && (g_grabStickHand == 2 || EngineGrabKeepAvailable());
    const float a = std::abs(dom.stickY);
    float v = 0;
    if (on && g_mouseStick != 2 && a > 0.2f && a > std::abs(dom.stickX))
        v = std::copysign((a - 0.2f) / 0.8f, dom.stickY) * std::clamp(c.grabStickSpeed, 0.1f, 10.0f);
    EngineSetGrabStick(on && g_grabStickHand == 1 ? v : 0.0f);
    g_offGrabVel = on && g_grabStickHand == 2 ? v : 0.0f;
    if (on) latch = true;
    else if (a < 0.3f) latch = false;
    return on || latch;
}
} // namespace

void SetOffFarHold(bool holding) { g_offFarHold = holding; }

float OffGrabStick() { return g_offGrabVel; }

// --- Dual wield (engine.cpp, DualEye: Total Overdose, one gun in each hand) ----------------------
// Each press of a trigger is ONE click of the left mouse button for that hand's gun (1 = dominant,
// 0 = the other); the engine gives that click's shot to that gun. The game fires once per update at
// most and the engine must know whose shot it is, so the clicks go one at a time: the down (the engine
// is told which gun: EngineDualClickBegin), once the game handled it (one update after it reached the
// window: DualWatch) the up, once that reached the game and one more update began, the next click.
// About one shot per two stereo pairs (~22 per second at 90 Hz); both triggers pressed together give
// two shots ~45 ms apart. A press waiting longer than 0.5 s is dropped; a step whose message never
// reaches the game (0.3 s) ends the click. (Those times count presents of 50 ms at most: one long
// frame - a hitch - does not end a click by itself; and a message of ours still held back for the
// frozen frame - g_deferred - has not been missed.)
std::deque<int> g_dualQueue;           // guns of presses waiting
int g_dualStep = 0;                    // 0 idle, 1 down posted, 2 down reached the game, 3 up posted, 4 up reached it
float g_dualStepT = 0;
unsigned long long g_dualStepFrame = 0;
bool g_dualTrig[2] = {};               // each trigger held (hysteresis), by gun
bool g_dualOn = false;                 // the triggers were the guns in the last update
float g_dualQueueT = 0;                // age of the oldest waiting press
int g_dualLogs = 0;

void DualEnd(const char* why)
{
    if (g_dualStep != 0) {
        g_dualExpect = 0;
        EngineDualClickEnd();
        if (g_dualStep == 1 || g_dualStep == 2) SetAct(A_UseTool, false);
        if (why && g_dualLogs < 60) {
            ++g_dualLogs;
            Log("Input: dual: click ended at step %d (%s)", g_dualStep, why);
        }
    }
    g_dualStep = 0;
    g_dualStepT = 0;
}

// Not the guns this update (another tool, a vehicle, menus...): any click in flight ends.
void DualOff()
{
    if (g_dualStep != 0) DualEnd("dual wield stopped");
    g_dualQueue.clear();
    g_dualTrig[0] = g_dualTrig[1] = false;
}

// offOk: the off hand's gun can fire (its copy is shown, the game's grab holds nothing).
void DualClicks(float domTrigger, float offTrigger, bool offOk, float dt)
{
    const float v[2] = { offTrigger, domTrigger };
    for (int gun = 0; gun < 2; ++gun) {
        const bool down = Hyst(g_dualTrig[gun], v[gun]);
        if (down && !g_dualTrig[gun] && (gun == 1 || offOk) && g_dualQueue.size() < 4) {
            if (g_dualQueue.empty()) g_dualQueueT = 0;
            g_dualQueue.push_back(gun);
        }
        g_dualTrig[gun] = down;
    }
    if (!offOk) {
        // (the off hand's gun can no longer fire - the game's grab took an object, which the left button
        // would throw; its copy hidden: its presses still waiting are dropped)
        const size_t n = g_dualQueue.size();
        g_dualQueue.erase(std::remove(g_dualQueue.begin(), g_dualQueue.end(), 0), g_dualQueue.end());
        if (g_dualQueue.size() != n && g_dualLogs < 60) {
            ++g_dualLogs;
            Log("Input: dual: %d press(es) of the off hand's gun dropped (it can no longer fire: an object held, "
                "its copy hidden)", (int)(n - g_dualQueue.size()));
        }
    }
    dt = std::min(dt, 0.05f);   // (see the comment above g_dualQueue)
    g_dualQueueT += dt;
    if (!g_dualQueue.empty() && g_dualQueueT > 0.5f) {
        if (g_dualLogs < 60) {
            ++g_dualLogs;
            Log("Input: dual: %d press(es) dropped (waited more than 0.5 s)", (int)g_dualQueue.size());
        }
        g_dualQueue.clear();
    }
    g_dualStepT += dt;
    const unsigned long long frame = EngineFrameCounter();
    switch (g_dualStep) {
    case 1:
        if (g_dualSeen == 1) {
            g_dualStep = 2;
            g_dualStepFrame = g_dualSeenFrame;
            g_dualStepT = 0;
        }
        break;
    case 2:
        // (one game update began since the down reached it: that update saw the press; no update for
        // 0.3 s: released anyway)
        if (frame > g_dualStepFrame || g_dualStepT > 0.3f) {
            g_dualSeen = 0;
            g_dualExpect = 2;
            SetAct(A_UseTool, false);
            g_dualStep = 3;
            g_dualStepT = 0;
        }
        break;
    case 3:
        if (g_dualSeen == 2) {
            g_dualStep = 4;
            g_dualStepFrame = g_dualSeenFrame;
            g_dualStepT = 0;
        }
        break;
    case 4:
        // (one more update began: the next down cannot reach the game in the same batch as this up)
        if (frame > g_dualStepFrame) DualEnd(nullptr);
        break;
    default:
        break;
    }
    // (steps 1 and 3: not while messages are held back for the frozen frame - ours may be among them)
    if (((g_dualStep == 1 || g_dualStep == 3) && g_deferred.empty() && g_dualStepT > 0.3f) ||
        (g_dualStep == 4 && g_dualStepT > 0.3f))
        DualEnd(g_dualStep == 1   ? "the down never reached the game"
                : g_dualStep == 3 ? "the up never reached the game"
                                  : "no game update after the up");
    if (g_dualStep != 0) return;
    if (g_actDown[A_UseTool]) {
        // (held from before - pressed by the usual controls just before dual wield began, the trigger
        // then latched: released first, even with no press waiting; the left button must not stay down)
        SetAct(A_UseTool, false);
        return;
    }
    if (g_dualQueue.empty()) return;
    const int gun = g_dualQueue.front();
    g_dualQueue.pop_front();
    g_dualQueueT = 0;
    EngineDualClickBegin(gun);
    g_dualSeen = 0;
    g_dualLegacy = !g_mouseRaw || !g_noLegacy;
    g_dualExpect = 1;
    SetAct(A_UseTool, true);
    g_dualStep = 1;
    g_dualStepT = 0;
    static unsigned clicks = 0;
    if (++clicks <= 20 || (Cfg().debugLua && clicks <= 2000))
        Log("Input: dual: click %u for the %s hand's gun (%s, %d waiting)", clicks, gun ? "dominant" : "off",
            g_dualLegacy ? "legacy button" : "raw input only", (int)g_dualQueue.size());
}

void Update(Mode mode, const HandState& left, const HandState& right, bool uiOpen, float dt)
{
    g_vrFocus = mode != Mode::Off;   // (see its comment)
    UpdateGameFocus();
    if (g_focusLossHidden) {
        // A deactivation kept from the game (see g_vrFocus): it no longer clears its keys, so the
        // keys of a real Alt-Tab / Windows key, released in the other window, are released here
        // (unless the keys panel holds them).
        g_focusLossHidden = false;
        if (g_latched.empty())
            for (WORD vk : { (WORD)VK_MENU, (WORD)VK_TAB, (WORD)VK_LWIN, (WORD)VK_RWIN }) SendKey(vk, false);
    }
    // (set again below, on foot; a press in progress ends outside gameplay on foot)
    g_offGripShown = 0;
    if (mode != Mode::Gameplay || uiOpen || g_inVehicle) g_offGripState = g_offTrigState = 0;
    if (mode != Mode::Gameplay || uiOpen) {
        EngineSetGrabStick(0);   // (in gameplay: GrabStick, below)
        g_offGrabVel = 0;
    }
    // Desktop mouse for the settings window.
    g_blockCursor = uiOpen && Cfg().showDesktopUi;
    if (uiOpen && g_hwnd) {
        POINT p;
        RECT rc;
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
        GetClientRect(g_hwnd, &rc);
        float sx = 1, sy = 1;
        // The UI is composited in back-buffer pixels; convert from window pixels.
        const VrStatus& st = VrGetStatus();
        if (rc.right > 0 && rc.bottom > 0 && st.backbufferW > 0) {
            sx = st.backbufferW / (float)rc.right;
            sy = st.backbufferH / (float)rc.bottom;
        }
        bool fg = GetForegroundWindow() == g_hwnd;
        bool down = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
        Ui::SetDesktopMouse(p.x * sx, p.y * sy, down, g_wheelAccum);
        g_wheelAccum = 0;
    }

    {
        // Diagnostics: another window in front of the game (it may then lose held buttons).
        static HWND lastFg = nullptr;
        static int fgLogs = 0;
        HWND fg = GetForegroundWindow();
        if (fg != lastFg && fgLogs < 60) {
            ++fgLogs;
            char cls[96] = {};
            if (fg) GetClassNameA(fg, cls, sizeof(cls) - 1);
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            char exe[MAX_PATH] = {};
            if (HANDLE ph = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) {
                DWORD n = sizeof(exe) - 1;
                QueryFullProcessImageNameA(ph, 0, exe, &n);
                CloseHandle(ph);
            }
            const char* base = strrchr(exe, '\\');
            Log("Input: window in front: %s (%s, %s)", fg == g_hwnd ? "the game" : "another window", cls,
                base ? base + 1 : exe);
        }
        lastFg = fg;
    }
    if (mode != g_lastMode) {
        DualOff();
        g_dualOn = false;
        // (a pause press in progress is kept: released here, its pulse would press Esc again)
        for (int i = 0; i < A_Count; ++i)
            if (i != A_Pause || g_pulse[i] <= 0) SetAct((Act)i, false);
        g_lastMode = mode;
        g_mapLatch = true;
    }
    // (set again below, in gameplay, while a stick acts as the mouse)
    if (!g_mouseStick) {
        g_virtValid = false;
        g_mouseRest[0] = g_mouseRest[1] = 0;
    }
    g_mouseStick = 0;
    UpdatePcKeys(dt, mode != Mode::Off && Ui::KeysShown());
    // The VR inventory only works on foot: hidden for now in menus, vehicles, the map, the
    // settings, and while a mod shows its camera away from the player (plane, drone... views:
    // looking down at the target must not show the belt panel); it comes back afterwards
    // unless R3 hid it.
    Inventory::Update(dt, mode == Mode::Gameplay && !g_inVehicle && !uiOpen && !ModCameraView());
    if (mode == Mode::Off) return;

    const bool lh = Cfg().leftHanded;
    HandState dom = lh ? left : right;   // dominant hand (aims, uses tool)
    HandState off = lh ? right : left;
    const float rawDomTrigger = dom.trigger, rawDomGrip = dom.squeeze;   // (diagnostics, below)
    // Our floating panels (vr.cpp, UpdatePanelGrips): a grip pressed while that hand's ray was on
    // one moves the panel, and stays the panel's until it is released - never the game's grab, the
    // off hand's grab or crouch (also through the vehicle and mods' window mappings below); the
    // trigger of the hand moving a panel does nothing either (a whole-fist squeeze), until it is
    // let go after the panel was released.
    const int domIdx = lh ? 0 : 1, offIdx = 1 - domIdx;
    if (VrPanelOwnsGrip(domIdx)) dom.squeeze = 0;
    if (VrPanelOwnsGrip(offIdx)) off.squeeze = 0;
    if (VrPanelMasksTrigger(domIdx)) dom.trigger = 0;
    if (VrPanelMasksTrigger(offIdx)) off.trigger = 0;
    // Dominant stick click: short = shows / hides the VR inventory (on foot; in a vehicle the keys panel),
    // held 0.5 s = keys panel (keyboard, mouse, keys of the active mods). A click made
    // while the stick is pushed (turning, changing tools) is an accident: ignored.
    static float domClickHeld = -1;   // (< 0: not pressed)
    static bool domClickMoved = false;
    bool domTap = false;
    if (dom.stickClick) {
        if (domClickHeld < 0) { domClickHeld = 0; domClickMoved = false; }
        if (std::abs(dom.stickX) > 0.5f || std::abs(dom.stickY) > 0.5f) domClickMoved = true;
        const float before = domClickHeld;
        domClickHeld += dt;
        if (!domClickMoved && before < 0.5f && domClickHeld >= 0.5f && mode != Mode::Off) {
            Ui::ToggleKeys();
            if (Ui::Visible()) VrPlaceUiPanel();
        }
    } else if (domClickHeld >= 0) {
        domTap = !domClickMoved && domClickHeld < 0.5f && mode != Mode::Off;
        domClickHeld = -1;
    }
    // Getting in / out of a vehicle changes what triggers and grips do: one still
    // held from before counts as released until it is let go.
    const bool remap = g_inVehicle != g_latchVehicle;
    g_latchVehicle = g_inVehicle;
    float* analog[4] = { &dom.trigger, &off.trigger, &dom.squeeze, &off.squeeze };
    for (int i = 0; i < 4; ++i) {
        if (remap && *analog[i] > 0.2f) g_analogLatch[i] = true;
        if (*analog[i] < 0.15f) g_analogLatch[i] = false;
        if (g_analogLatch[i]) *analog[i] = 0;
    }
    // X opens / closes the map. Opening it changes the input mode: X must be
    // released first, or the map key is pressed again and the map toggles back.
    if (!off.primary) g_mapLatch = false;
    const bool mapKey = off.primary && !g_mapLatch;

    // Left menu button: short press = pause (Esc), long press = settings panel.
    if (left.menu) {
        if (!g_menuWasDown) { g_menuHeld = 0; g_menuLongFired = false; }
        g_menuHeld += dt;
        if (!g_menuLongFired && g_menuHeld > 0.6f) {
            Ui::Toggle();
            if (Ui::Visible()) VrPlaceUiPanel();
            g_menuLongFired = true;
        }
    } else if (g_menuWasDown && !g_menuLongFired) {
        Pulse(A_Pause);
    }
    g_menuWasDown = left.menu;
    for (int i = 0; i < A_Count; ++i) {
        if (g_pulse[i] > 0) {
            SetAct((Act)i, true);
            g_pulse[i] -= dt;
            if (g_pulse[i] <= 0) SetAct((Act)i, false);
        }
    }

    // Left stick click: hold = recenter; short press = quick key (the last key
    // used on the keys panel, e.g. Q = previous shell of the AC130 mod). A click
    // made while pushing the stick (walking, aiming with it) is not a quick key.
    if (off.stickClick) {
        if (std::abs(off.stickX) > 0.5f || std::abs(off.stickY) > 0.5f) g_clickMoved = true;
        g_recenterHeld += dt;
        if (g_recenterHeld > 0.8f && g_recenterHeld - dt <= 0.8f) VrRequestRecenter();
    } else {
        if (g_recenterHeld > 0 && g_recenterHeld < 0.35f && !g_clickMoved && mode == Mode::Gameplay && !uiOpen)
            TapKey(PcKeys::ByName(Cfg().quickKey.c_str()));
        g_recenterHeld = 0;
        g_clickMoved = false;
    }
    // (short click outside gameplay on foot - menus, vehicle, settings open: keys panel)
    if (domTap && !(mode == Mode::Gameplay && !g_inVehicle && !uiOpen)) {
        Ui::ToggleKeys();
        if (Ui::Visible()) VrPlaceUiPanel();
        domTap = false;
    }

    if (uiOpen) {
        // The laser pointer drives the settings panel; the game gets nothing.
        DualOff();
        g_dualOn = false;
        for (int i = 0; i < A_Count; ++i)
            if (i != A_Pause || g_pulse[i] <= 0) SetAct((Act)i, false);
        return;
    }

    if (g_releaseMoveKeys.exchange(false)) {
        // A movement key the game still sees held on foot (lost key-up, the keys panel...):
        // released, with what this mod thought it held (diagnostics).
        static int logs = 0;
        if (logs < 20) {
            ++logs;
            Log("Input: the game saw a movement key held on foot - released W/A/S/D and arrows (this mod held: forward %d back %d left %d right %d, vehicle %d)",
                g_actDown[A_Forward] ? 1 : 0, g_actDown[A_Backward] ? 1 : 0, g_actDown[A_Left] ? 1 : 0,
                g_actDown[A_Right] ? 1 : 0, g_inVehicle ? 1 : 0);
        }
        if (!g_inVehicle) {
            for (Act a : { A_Forward, A_Backward, A_Left, A_Right }) g_actDown[a] = false;
            SendKeyPos('W', 0x11, false);
            SendKeyPos('S', 0x1F, false);
            SendKeyPos('A', 0x1E, false);
            SendKeyPos('D', 0x20, false);
            SendKeyPos('Z', 0x2C, false);   // (the AZERTY letters at those places, in case a
            SendKeyPos('Q', 0x10, false);   //  physical keyboard pressed them)
            SendKey(VK_UP, false);
            SendKey(VK_DOWN, false);
            SendKey(VK_LEFT, false);
            SendKey(VK_RIGHT, false);
        }
    }
    if (mode == Mode::Gameplay) {
        // A stick as the mouse while a mod moves the camera itself (plane, drone,
        // turret: camerax / cameray aim it) or when forced from the keys panel. In a
        // vehicle the movement stick steers: the rotation stick is used. (First: the
        // clicks below land on its cursor.) The script camera still counts 0.25 s
        // after it was last seen: one frame without it (a mod setting it at a lower
        // rate than the frames) must not give the stick back to walking.
        g_scriptCamHold = EngineScriptCamera() ? 0.25f : std::max(0.0f, g_scriptCamHold - dt);
        if (g_mouseAlways || (Cfg().modMouse == 0 && g_scriptCamHold > 0))
            g_mouseStick = g_inVehicle || Cfg().modMouseStick == 1 ? 2 : 1;
        if (g_mouseStick) {
            const HandState& h = g_mouseStick == 1 ? off : dom;
            StickMouse(h.stickX, h.stickY, dt);
        }
        SetAct(A_MenuClick, false);
        SetAct(A_MenuBack, false);
        bool swing = g_swingTime > 0;
        g_swingTime -= dt;
        SetAct(A_Jump, dom.primary);
        SetAct(A_Flashlight, dom.secondary);
        SetAct(A_Map, mapKey);
        // On foot the VR inventory picks a tool with the trigger of the hand pointing at it
        // (Input::PointerHand). The other hand's trigger then does not crouch, nor reach
        // anything else below; the dominant hand's: Inventory::CapturesTrigger.
        if (!g_inVehicle && Inventory::CapturesOffTrigger()) off.trigger = 0;
        if (g_inVehicle) {
            // Driving, like a console game: right trigger = accelerate (W), left
            // trigger = brake / reverse (S), left stick = steer (A / D), A =
            // handbrake (space), Y = get out (E). Grips: crane arm up (left mouse
            // button) / down (right mouse button).
            // (the game's throttle is on/off: a pulsed key did not move the vehicle)
            bool brake = Hyst(g_actDown[A_Backward], off.trigger, 0.4f, 0.25f);
            bool gas = Hyst(g_actDown[A_Forward] || (g_actDown[A_Backward] && dom.trigger > 0.4f), dom.trigger, 0.4f, 0.25f);
            bool steer = Pwm(off.stickX, g_pwmPhase[1], dt);
            SetAct(A_Forward, gas && !brake);
            SetAct(A_Backward, brake);
            SetAct(A_Right, steer && off.stickX > 0);
            SetAct(A_Left, steer && off.stickX < 0);
            SetAct(A_UseTool, Hyst(g_actDown[A_UseTool], dom.squeeze, 0.6f, 0.4f));
            SetAct(A_Grab, Hyst(g_actDown[A_Grab], off.squeeze, 0.6f, 0.4f));
            SetAct(A_Interact, off.secondary);
            SetAct(A_Crouch, false);
            g_keyRefreshLeft = 3;
            DualOff();
            g_dualOn = false;
        } else {
            // A mod's interactive window (UiMakeInteractive: the revolver's ammo menu...): the
            // mouse follows the hand pointing at it (engine.cpp, ModUiPointer). That hand's
            // trigger is the left mouse button (held = drag) and its grip the right one; pulling
            // the other trigger while aiming at the window gives it the pointer. Meanwhile the
            // left trigger does not crouch, and the off hand's grip does not grab while it
            // points. (No window: unchanged.)
            if (EngineModUiShown()) {
                const int uiHand = EngineModUiHand();
                if (uiHand >= 0 && (uiHand == 0) != lh) {   // (the off hand points)
                    dom.trigger = off.trigger;
                    dom.squeeze = off.squeeze;
                    off.squeeze = 0;
                }
                off.trigger = 0;
            }
            // Dual wield (engine.cpp, DualEye: Total Overdose, one gun in each hand), as the player
            // asked: each trigger fires its own hand's gun, one click per press (DualClicks); the off
            // hand's grip crouches (that hand holds a gun: it grabs nothing), its trigger no longer does.
            // Not while a mod's window takes the triggers. A trigger held when this starts or stops
            // counts as released until let go (it neither fires nor crouches).
            const bool dualOn = EngineDualReady() && !EngineModUiShown();
            if (dualOn != g_dualOn) {
                for (int i = 0; i < 2; ++i)
                    if (*analog[i] > 0.2f) {
                        g_analogLatch[i] = true;
                        *analog[i] = 0;
                    }
                g_dualOn = dualOn;
                static int logs = 0;
                if (logs < 40) {
                    ++logs;
                    Log(dualOn ? "Input: dual wield on - each trigger fires its own hand's gun, the off hand's grip "
                                 "crouches"
                               : "Input: dual wield off - triggers and grips as usual");
                }
            }
            // (pointing at the VR inventory, the trigger picks a tool instead of using one)
            if (dualOn) {
                // (the game's grab holds an object: the left button would throw it - the off gun waits)
                const bool offOk = EngineDualGunReady(0) && !g_playerGrabbing;
                DualClicks(Inventory::CapturesTrigger() ? 0.0f : dom.trigger, off.trigger, offOk, dt);
            } else {
                DualOff();
                // (the game's sledge: a light, short pull is one whole strike - SledgeUse; not while the
                // game's grab holds an object: the left button then throws it, at the usual firm pull)
                const bool sledge = Cfg().sledgeTap && EngineHoldingSledge() && !g_playerGrabbing;
                bool use = SledgeUse(sledge, dom.trigger, Inventory::CapturesTrigger(), dt);
                if (!sledge) use = Hyst(g_actDown[A_UseTool], dom.trigger);
                SetAct(A_UseTool, (use || swing) && !Inventory::CapturesTrigger());
            }
            // (R3 shows / hides it; shown but far from where the player looks - behind after turning
            // round - R3 brings it back in front of the body instead: VrInventoryRecall)
            if (domTap && !ModCameraView()) {
                if (VrInventoryRecall()) Inventory::RequestPlace();
                else Inventory::Toggle();
            }
            // (dual wield: Total Overdose turns the game's grab button - the right mouse button - into its
            // "dodge", a ragdoll rolling on the ground with a third-person camera, unwanted in VR: the
            // dominant grip sends nothing while one gun is in each hand)
            SetAct(A_Grab, !dualOn && Hyst(g_actDown[A_Grab], dom.squeeze, 0.6f, 0.4f));
            // The off hand (left by default), as the player asked:
            //   - grip = grab with the physical off hand, like the dominant hand's grip (the game's
            //     grab): what it touches or what its laser points at (the Lua mod picks it; nothing
            //     there, nothing happens). With the physical hand or its grab turned off, the grip
            //     crouches as before (never a dead button);
            //   - trigger (L2, the same trigger reverses in a vehicle) = crouch, while held;
            //   - Y = interact (E), also gets into a vehicle.
            //   (dual wield: the trigger fires that hand's gun, the grip crouches - see above)
            const Settings& cfgH = Cfg();
            const bool handGrab = cfgH.offArm && cfgH.offHandPhysics && cfgH.offGrab && !dualOn;
            g_offTrigState = !dualOn && Hyst(g_offTrigState != 0, off.trigger) ? 1 : 0;   // (1 = crouch press)
            SetAct(A_Interact, off.secondary);
            const bool gripDown = Hyst(g_offGripState != 0, off.squeeze, 0.6f, 0.4f);
            if (!gripDown) {
                g_offGripState = 0;
            } else if (g_offGripState == 0) {
                g_offGripState = handGrab ? 2 : 1;
                if (g_offGripState == 2 && g_offGrabLogs < 20) {
                    ++g_offGrabLogs;
                    Log("Input: off-hand grip = grab with the physical hand (left trigger = crouch, Y = interact)");
                }
            }
            g_offGripShown = g_offGripState;
            SetAct(A_Crouch, g_offTrigState == 1 || g_offGripState == 1);
            // Diagnostics: the dominant hand's tool / grab presses, at each change, with what
            // decided them (a press that ends while the trigger or grip is still held: a mod's
            // window took the hand, the VR inventory, a latch, the game window not in front...).
            static bool diagUse = false, diagGrab = false;
            static int diagLogs = 0;
            if ((g_actDown[A_UseTool] != diagUse || g_actDown[A_Grab] != diagGrab) && diagLogs < 300) {
                ++diagLogs;
                Log("Input: dominant hand tool %d grab %d - trigger %.2f grip %.2f (controller %.2f %.2f), "
                    "mod window %d (hand %d), inventory takes the trigger %d, latched %d%d, panel grip %d trigger %d (hand moving a panel %d), "
                    "game window in front %d (game active %d)",
                    g_actDown[A_UseTool] ? 1 : 0, g_actDown[A_Grab] ? 1 : 0, dom.trigger, dom.squeeze, rawDomTrigger,
                    rawDomGrip, EngineModUiShown() ? 1 : 0, EngineModUiHand(), Inventory::CapturesTrigger() ? 1 : 0,
                    g_analogLatch[0] ? 1 : 0, g_analogLatch[2] ? 1 : 0, VrPanelOwnsGrip(domIdx) ? 1 : 0,
                    VrPanelMasksTrigger(domIdx) ? 1 : 0, VrPanelHoldHand(), GetForegroundWindow() == g_hwnd ? 1 : 0,
                    g_gameActive ? 1 : 0);
            }
            diagUse = g_actDown[A_UseTool];
            diagGrab = g_actDown[A_Grab];
            for (Act a : { A_Forward, A_Backward, A_Left, A_Right }) SetAct(a, false);
            // Safety net: on foot the mod never holds the driving keys; repeat their
            // release now and then, so the game can never keep one "stuck" down.
            if (g_keyRefreshLeft > 0 && (g_keyRefresh -= dt) <= 0) {
                g_keyRefresh = 1.0f;
                --g_keyRefreshLeft;
                SendKeyPos('W', 0x11, false);
                SendKeyPos('S', 0x1F, false);
                SendKeyPos('A', 0x1E, false);
                SendKeyPos('D', 0x20, false);
            }
        }

        // Dominant stick up/down cycles tools (mouse wheel), with repeat. It does not
        // show the VR inventory (only R3 does), unless the inventoryOnSwitch option asks
        // for a short display-only glimpse. While a hand holds an object at a distance it
        // moves that object instead (grabKeepDistance, GrabStick).
        int dir = dom.stickY > 0.7f ? 1 : (dom.stickY < -0.7f ? -1 : 0);
        if (std::abs(dom.stickX) > std::abs(dom.stickY) || g_mouseStick == 2) dir = 0;
        if (GrabStick(dom)) dir = 0;
        if (dir != 0) {
            if (!g_inVehicle) Inventory::ShowBrief();
            // (in a mod's camera view the wheel is usually its zoom - the AC130's: the
            // steps follow faster, and the engine eases the zoom between them: ModZoom)
            const bool modView = ModCameraView();
            if (dir != g_lastScrollDir) {
                SendWheel(dir);
                g_scrollRepeat = modView ? 0.3f : 0.45f;
            } else if ((g_scrollRepeat -= dt) <= 0) {
                SendWheel(dir);
                g_scrollRepeat = modView ? 0.12f : 0.2f;
            }
        }
        g_lastScrollDir = dir;
    } else if (mode == Mode::Map) {
        DualOff();
        g_dualOn = false;
        // 3D level map: left stick moves over the map, right stick up/down zooms,
        // X closes it.
        for (Act a : { A_UseTool, A_Grab, A_Interact, A_Crouch, A_Jump, A_Flashlight, A_MenuClick, A_MenuBack,
                       A_Forward, A_Backward, A_Left, A_Right })
            SetAct(a, false);
        SetAct(A_Map, mapKey);
        EngineSetMapInput(off.stickX, off.stickY, std::abs(dom.stickY) > std::abs(dom.stickX) ? dom.stickY : 0.0f);
    } else if (mode == Mode::Screen) {
        DualOff();
        g_dualOn = false;
        for (Act a : { A_UseTool, A_Grab, A_Interact, A_Crouch, A_Jump, A_Flashlight, A_Map, A_Forward, A_Backward, A_Left,
                       A_Right })
            SetAct(a, false);
        // Move the game cursor to where the controller points on the virtual screen.
        // The game follows WM_MOUSEMOVE, so this works even if another desktop
        // window has the focus (the player is in the headset); the real Windows
        // cursor is only moved while the game is the foreground window.
        if (g_screenU >= 0 && g_hwnd) {
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            // The game reads mouse positions in the pixels of the size it draws at, which is
            // the window's client size - except when it set itself to another size (exclusive
            // fullscreen at the monitor's resolution, refused by the mod: the picture is
            // stretched over the window and a client position would land elsewhere).
            int gw = 0, gh = 0;
            if (EngineGameSize(gw, gh)) { rc.right = gw; rc.bottom = gh; }
            POINT p = { (LONG)(g_screenU * rc.right), (LONG)(g_screenV * rc.bottom) };
            // Small dead zone: the hand's tremor must not make menu items flicker.
            const LONG dead = std::max<LONG>(2, rc.right / 800);
            if (g_lastPointer.x < 0 || std::abs(p.x - g_lastPointer.x) > dead || std::abs(p.y - g_lastPointer.y) > dead) {
                g_lastPointer = p;
                PostMessageW(g_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
                // The game window is usually larger than the desktop (VR resolution):
                // Windows would clamp the real cursor to the screen edge and send the
                // game that other position too, so it is only moved when it fits.
                POINT s = p;
                ClientToScreen(g_hwnd, &s);
                if (GetForegroundWindow() == g_hwnd && MonitorFromPoint(s, MONITOR_DEFAULTTONULL))
                    Real_SetCursorPos(s.x, s.y);
            }
        }
        // The click: the trigger of the hand that points (PointerHand - the hand whose trigger was pulled
        // on the screen last: vr.cpp gives it the pointer earlier in this present, so that first press
        // already clicks at its own ray). The other hand's trigger clicks nothing (it takes the pointer
        // only with its ray on the screen), nor does a press begun with the pointer hand's ray beside the
        // screen (it went to the real Windows cursor, wherever that was: the game's cursor is still where
        // the ray left it, unseen). Without offHandPointer: either trigger, at the dominant hand's ray.
        static bool menuBeside = false;   // the current press began with the ray beside the screen
        static int menuHand = -1;         // the pointer hand that press belongs to
        const int ptr = PointerHand();
        if (ptr != menuHand) {
            menuHand = ptr;
            menuBeside = false;   // (a press the other hand began beside the screen is not this hand's)
        }
        const float menuTrig = Cfg().offHandPointer ? (ptr == domIdx ? dom.trigger : off.trigger)
                                                    : std::max(dom.trigger, off.trigger);
        const bool menuWas = g_actDown[A_MenuClick] || menuBeside;
        const bool menuDown = Hyst(menuWas, menuTrig);
        if (menuDown && !menuWas) {
            menuBeside = g_screenU < 0;
            static int clickLogs = 0;   // (diagnostics: who clicked where; a few lines only)
            if (clickLogs < 60) {
                ++clickLogs;
                if (menuBeside)
                    Log("Input: menu press (pointer hand %s) began with its ray beside the screen - no click",
                        ptr ? "right" : "left");
                else
                    Log("Input: menu click by the %s hand's trigger at %ld, %ld (game pixels)",
                        !Cfg().offHandPointer ? "dominant or other" : ptr ? "right" : "left", (long)g_lastPointer.x,
                        (long)g_lastPointer.y);
            }
        }
        if (!menuDown) menuBeside = false;
        SetAct(A_MenuClick, menuDown && !menuBeside);
        SetAct(A_MenuBack, dom.secondary || off.secondary);
        SetAct(A_Map, mapKey);   // X closes the level map (shown on the virtual screen)
        int dir = dom.stickY > 0.7f ? 1 : (dom.stickY < -0.7f ? -1 : 0);
        if (dir != 0 && ((g_scrollRepeat -= dt) <= 0 || dir != g_lastScrollDir)) {
            SendWheel(dir);
            g_scrollRepeat = 0.15f;
        }
        g_lastScrollDir = dir;
    }
}

void InstallHooks()
{
    if (auto p = HookIAT("USER32.dll", "GetRawInputData", (void*)Hook_GetRawInputData)) Real_GetRawInputData = (PFN_GetRawInputData)p;
    if (auto p = HookIAT("USER32.dll", "RegisterRawInputDevices", (void*)Hook_RegisterRawInputDevices)) Real_RegisterRawInputDevices = (PFN_RegisterRawInputDevices)p;
    if (auto p = HookIAT("USER32.dll", "SetCursorPos", (void*)Hook_SetCursorPos)) Real_SetCursorPos = (PFN_SetCursorPos)p;
    if (auto p = HookIAT("USER32.dll", "DefWindowProcA", (void*)Hook_DefWindowProcA)) Real_DefWindowProcA = (PFN_DefWindowProc)p;
    if (auto p = HookIAT("USER32.dll", "DefWindowProcW", (void*)Hook_DefWindowProcW)) Real_DefWindowProcW = (PFN_DefWindowProc)p;
}

} // namespace Input
