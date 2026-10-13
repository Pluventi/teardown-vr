#include "d3d_hooks.h"
#include "gpu_trace.h"
#include "hooks.h"
#include "log.h"
#include "particles_vr.h"
#include "perf.h"
#include "prof.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>

bool g_traceGpuSubmits = false;

namespace {

using PFN_CreateDXGIFactory  = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);

PFN_CreateDXGIFactory  Real_CreateDXGIFactory;
PFN_CreateDXGIFactory  Real_CreateDXGIFactory1;
PFN_CreateDXGIFactory2 Real_CreateDXGIFactory2;
PFN_CreateSwapChain Real_CreateSwapChain;
PFN_CreateSwapChainForHwnd Real_CreateSwapChainForHwnd;
PFN_Present Real_Present;
PFN_Present1 Real_Present1;
PFN_ResizeBuffers Real_ResizeBuffers;
PFN_ResizeBuffers1 Real_ResizeBuffers1;

std::mutex g_mutex;
std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> g_queues;
thread_local bool t_inPresent = false;

void** VTable(void* obj) { return *(void***)obj; }

// Reverse-engineering aid: where does the game submit its GPU work from?
using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
PFN_ExecuteCommandLists Real_ExecuteCommandLists;
std::atomic<int> g_eclLogged{ 0 };
std::atomic<int> g_eclThisFrame{ 0 };

void STDMETHODCALLTYPE Hook_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (!t_inPresent && g_traceGpuSubmits) {
        if (n) GpuTraceInstall(lists[0]);
        g_eclThisFrame++;
        if (g_eclLogged < 12) {
            ++g_eclLogged;
            char tag[64];
            snprintf(tag, sizeof(tag), "ecl(q=%p,n=%u)", (void*)q, n);
            LogStack(tag);
        }
    }
    // ([Debug] perfTrace + [Performance] perfDetail 4: on the game's queue, between two timestamps of ours -
    // the GPU's busy and idle time per frame, perf.cpp)
    if (Perf::GpuSubmit(q, n, lists, t_inPresent, Real_ExecuteCommandLists)) return;
    Real_ExecuteCommandLists(q, n, lists);
}

ID3D12CommandQueue* QueueFor(IDXGISwapChain* sc)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_queues.find(sc);
    return it == g_queues.end() ? nullptr : it->second;
}

void HandlePresent(IDXGISwapChain* sc, UINT& sync, UINT& flags)
{
    if (flags & DXGI_PRESENT_TEST) return;
    if (g_traceGpuSubmits) {
        static int frames = 0;
        if (frames++ < 6) Log("frame: %d game ExecuteCommandLists since last present", g_eclThisFrame.exchange(0));
    }
    ID3D12CommandQueue* queue = QueueFor(sc);
    if (!queue) return;
    Perf::GpuQueue(queue);   // (perfDetail 4: the queue whose submissions are timed)
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    OnGamePresent(sc3, queue, sync, flags);
    sc3->Release();
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!t_inPresent) {
        t_inPresent = true;
        GpuTraceOnPresent(true);
        { ProfScope p(P_OurPresent); HandlePresent(sc, sync, flags); }
        HRESULT hr;
        { ProfScope p(P_RealPresent); hr = Real_Present(sc, sync, flags); }
        { ProfScope p(P_PostPresent); OnGamePostPresent(); }
        if (!(flags & DXGI_PRESENT_TEST)) Perf::GpuFrameEnd();   // (perfDetail 4: this frame's submissions are all in)
        GpuTraceOnPresent(false);
        t_inPresent = false;
        return hr;
    }
    return Real_Present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    if (!t_inPresent) {
        t_inPresent = true;
        GpuTraceOnPresent(true);
        { ProfScope pr(P_OurPresent); HandlePresent(sc, sync, flags); }
        HRESULT hr;
        { ProfScope pr(P_RealPresent); hr = Real_Present1(sc, sync, flags, p); }
        { ProfScope pr(P_PostPresent); OnGamePostPresent(); }
        if (!(flags & DXGI_PRESENT_TEST)) Perf::GpuFrameEnd();   // (perfDetail 4: this frame's submissions are all in)
        GpuTraceOnPresent(false);
        t_inPresent = false;
        return hr;
    }
    return Real_Present1(sc, sync, flags, p);
}

void BeforeResize(IDXGISwapChain* sc)
{
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        OnGameBeforeResize(sc3);
        sc3->Release();
    }
}

void AfterResize(IDXGISwapChain* sc)
{
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        OnGameAfterResize(sc3);
        sc3->Release();
    }
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    // The size is the game's own choice (it derives it from the monitor size we
    // report). Never force another one: the game would render for one size and
    // present another.
    Log("ResizeBuffers(count=%u, %ux%u, fmt=%d, flags=0x%x)", count, w, h, fmt, flags);
    BeforeResize(sc);
    HRESULT hr = Real_ResizeBuffers(sc, count, w, h, fmt, flags);
    AfterResize(sc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
    const UINT* nodeMasks, IUnknown* const* queues)
{
    Log("ResizeBuffers1(count=%u, %ux%u, fmt=%d)", count, w, h, fmt);
    BeforeResize(sc);
    HRESULT hr = Real_ResizeBuffers1(sc, count, w, h, fmt, flags, nodeMasks, queues);
    AfterResize(sc);
    return hr;
}

void HookSwapchain(IDXGISwapChain* sc, IUnknown* device)
{
    ID3D12CommandQueue* queue = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        Log("Swapchain %p created on a non-D3D12 device, ignored", sc);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queues[sc] = queue; // keep the reference for the swapchain lifetime
    }
    void** vt = VTable(sc);
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        HookFunction(vt[8], (void*)Hook_Present, (void**)&Real_Present);
        HookFunction(vt[13], (void*)Hook_ResizeBuffers, (void**)&Real_ResizeBuffers);
        HookFunction(vt[22], (void*)Hook_Present1, (void**)&Real_Present1);
        HookFunction(vt[39], (void*)Hook_ResizeBuffers1, (void**)&Real_ResizeBuffers1);
        HookFunction(VTable(queue)[10], (void*)Hook_ExecuteCommandLists, (void**)&Real_ExecuteCommandLists);
        Log("Swapchain hooks installed");
    }
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** sc)
{
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    d.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    HRESULT hr = Real_CreateSwapChainForHwnd(f, device, hwnd, &d, fs, out, sc);
    Log("CreateSwapChainForHwnd(hwnd=%p, %ux%u, fmt=%d, buffers=%u, swap=%d, flags=0x%x) -> 0x%08x",
        hwnd, d.Width, d.Height, d.Format, d.BufferCount, d.SwapEffect, d.Flags, hr);
    if (SUCCEEDED(hr) && sc && *sc) HookSwapchain(*sc, device);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc)
{
    if (desc) desc->BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    HRESULT hr = Real_CreateSwapChain(f, device, desc, sc);
    Log("CreateSwapChain(%ux%u fmt=%d) -> 0x%08x", desc ? desc->BufferDesc.Width : 0, desc ? desc->BufferDesc.Height : 0,
        desc ? desc->BufferDesc.Format : 0, hr);
    if (SUCCEEDED(hr) && sc && *sc) HookSwapchain(*sc, device);
    return hr;
}

void HookFactory(void* factory)
{
    // ([Affichage] stereoParticles: the engine's pipeline creation is hooked before its renderer makes its shaders - the
    // game's code is decrypted by now; tried again at the next factory until done)
    ParticlesVrEarly();
    static bool done = false;
    if (done || !factory) return;
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(((IUnknown*)factory)->QueryInterface(IID_PPV_ARGS(&f2)))) return;
    done = true;
    void** vt = VTable(f2);
    HookFunction(vt[10], (void*)Hook_CreateSwapChain, (void**)&Real_CreateSwapChain);
    HookFunction(vt[15], (void*)Hook_CreateSwapChainForHwnd, (void**)&Real_CreateSwapChainForHwnd);
    f2->Release();
    Log("DXGI factory hooks installed");
}

HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void** out)
{
    HRESULT hr = Real_CreateDXGIFactory(riid, out);
    if (SUCCEEDED(hr)) HookFactory(*out);
    return hr;
}
HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** out)
{
    HRESULT hr = Real_CreateDXGIFactory1(riid, out);
    if (SUCCEEDED(hr)) HookFactory(*out);
    return hr;
}
HRESULT WINAPI Hook_CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    HRESULT hr = Real_CreateDXGIFactory2(flags, riid, out);
    if (SUCCEEDED(hr)) HookFactory(*out);
    return hr;
}

} // namespace

namespace {
// Custom render resolution: Teardown sizes its window and render targets from
// the monitor it runs on, so we report a monitor of the requested size.
using PFN_GetMonitorInfoA = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
PFN_GetMonitorInfoA Real_GetMonitorInfoA = GetMonitorInfoA;

BOOL WINAPI Hook_GetMonitorInfoA(HMONITOR mon, LPMONITORINFO info)
{
    BOOL ok = Real_GetMonitorInfoA(mon, info);
    UINT w = 0, h = 0;
    GetSwapchainSizeOverride(w, h);
    if (ok && w && h && info) {
        info->rcMonitor.right = info->rcMonitor.left + (LONG)w;
        info->rcMonitor.bottom = info->rcMonitor.top + (LONG)h;
        info->rcWork = info->rcMonitor;
        static bool logged = false;
        if (!logged) { logged = true; Log("Render resolution: monitor reported as %ux%u", w, h); }
    }
    return ok;
}
using PFN_QueryDisplayConfig = LONG(WINAPI*)(UINT32, UINT32*, DISPLAYCONFIG_PATH_INFO*, UINT32*, DISPLAYCONFIG_MODE_INFO*,
                                              DISPLAYCONFIG_TOPOLOGY_ID*);
using PFN_GetSystemMetrics = int(WINAPI*)(int);
using PFN_ChangeDisplaySettingsExA = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
PFN_QueryDisplayConfig Real_QueryDisplayConfig = QueryDisplayConfig;
PFN_GetSystemMetrics Real_GetSystemMetrics = GetSystemMetrics;
PFN_ChangeDisplaySettingsExA Real_ChangeDisplaySettingsExA = ChangeDisplaySettingsExA;

LONG WINAPI Hook_QueryDisplayConfig(UINT32 flags, UINT32* numPaths, DISPLAYCONFIG_PATH_INFO* paths, UINT32* numModes,
                                    DISPLAYCONFIG_MODE_INFO* modes, DISPLAYCONFIG_TOPOLOGY_ID* topo)
{
    LONG r = Real_QueryDisplayConfig(flags, numPaths, paths, numModes, modes, topo);
    UINT w = 0, h = 0;
    GetSwapchainSizeOverride(w, h);
    if (r == ERROR_SUCCESS && w && h && modes && numModes)
        for (UINT32 i = 0; i < *numModes; ++i)
            if (modes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                modes[i].sourceMode.width = w;
                modes[i].sourceMode.height = h;
            }
    return r;
}

int WINAPI Hook_GetSystemMetrics(int index)
{
    UINT w = 0, h = 0;
    GetSwapchainSizeOverride(w, h);
    if (w && h) {
        if (index == SM_CXSCREEN) return (int)w;
        if (index == SM_CYSCREEN) return (int)h;
    }
    return Real_GetSystemMetrics(index);
}

LONG WINAPI Hook_ChangeDisplaySettingsExA(LPCSTR device, DEVMODEA* mode, HWND hwnd, DWORD flags, LPVOID param)
{
    UINT w = 0, h = 0;
    GetSwapchainSizeOverride(w, h);
    if (w && h) {
        // Never change the real monitor mode to our virtual render resolution.
        Log("Render resolution: blocked a display mode change (fullscreen) - use borderless/windowed");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_ChangeDisplaySettingsExA(device, mode, hwnd, flags, param);
}
using PFN_SetWindowPos = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using PFN_CreateWindowExW = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
PFN_SetWindowPos Real_SetWindowPos = SetWindowPos;
PFN_CreateWindowExW Real_CreateWindowExW = CreateWindowExW;

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    UINT w = 0, h = 0;
    GetSwapchainSizeOverride(w, h);
    if (w && h && !(flags & SWP_NOSIZE) && (GetWindowLongW(hwnd, GWL_STYLE) & WS_POPUP)) {
        cx = (int)w;
        cy = (int)h;
    }
    return Real_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

HWND WINAPI Hook_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int cx, int cy, HWND parent,
                                 HMENU menu, HINSTANCE inst, LPVOID param)
{
    UINT w = 0, h = 0;
    GetSwapchainSizeOverride(w, h);
    if (w && h && (style & WS_POPUP)) {
        cx = (int)w;
        cy = (int)h;
        Log("Render resolution: game window created at %ux%u", w, h);
    }
    return Real_CreateWindowExW(ex, cls, name, style, x, y, cx, cy, parent, menu, inst, param);
}
} // namespace

// Teardown's "Plein écran" display mode makes the game draw at a resolution of the
// monitor's own mode list (e.g. 1920x1080 on a 1080p monitor), whatever size the window and the
// reported monitor have: the headset then got a 1920x1080 picture stretched over each
// eye (3024x3264), very blurred, and the menus' mouse positions no longer matched. The
// mod needs a window (and blocks the display mode change anyway), so a full-screen
// setting is changed to "Fenêtre sans bordure" (options.display.mode 2) before the game
// reads its options. (Plain file edit at load: no shell API under the loader lock.)
void FixFullscreenOption()
{
    wchar_t dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    const std::wstring path = std::wstring(dir) + L"\\Teardown\\options.xml";
    std::string xml;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        xml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const size_t d = xml.find("<display>");
    const size_t e = d == std::string::npos ? std::string::npos : xml.find("</display>", d);
    const char* full = "<mode value=\"0\"/>";
    const size_t m = d == std::string::npos ? std::string::npos : xml.find(full, d);
    if (e == std::string::npos || m == std::string::npos || m > e) return;
    xml.replace(m, strlen(full), "<mode value=\"2\"/>");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << xml;
    Log("Render resolution: the game's display mode was full screen (it then draws at the monitor's resolution: "
        "a blurred picture in the headset) - set to borderless window");
}

void D3DHooksInit()
{
    if (auto p = HookIAT("USER32.dll", "SetWindowPos", (void*)Hook_SetWindowPos))
        Real_SetWindowPos = (PFN_SetWindowPos)p;
    if (auto p = HookIAT("USER32.dll", "CreateWindowExW", (void*)Hook_CreateWindowExW))
        Real_CreateWindowExW = (PFN_CreateWindowExW)p;
    if (auto p = HookIAT("USER32.dll", "GetMonitorInfoA", (void*)Hook_GetMonitorInfoA))
        Real_GetMonitorInfoA = (PFN_GetMonitorInfoA)p;
    if (auto p = HookIAT("USER32.dll", "QueryDisplayConfig", (void*)Hook_QueryDisplayConfig))
        Real_QueryDisplayConfig = (PFN_QueryDisplayConfig)p;
    if (auto p = HookIAT("USER32.dll", "GetSystemMetrics", (void*)Hook_GetSystemMetrics))
        Real_GetSystemMetrics = (PFN_GetSystemMetrics)p;
    if (auto p = HookIAT("USER32.dll", "ChangeDisplaySettingsExA", (void*)Hook_ChangeDisplaySettingsExA))
        Real_ChangeDisplaySettingsExA = (PFN_ChangeDisplaySettingsExA)p;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) { Log("dxgi.dll not found"); return; }
    if (auto p = GetProcAddress(dxgi, "CreateDXGIFactory"))
        HookFunction((void*)p, (void*)Hook_CreateDXGIFactory, (void**)&Real_CreateDXGIFactory);
    if (auto p = GetProcAddress(dxgi, "CreateDXGIFactory1"))
        HookFunction((void*)p, (void*)Hook_CreateDXGIFactory1, (void**)&Real_CreateDXGIFactory1);
    if (auto p = GetProcAddress(dxgi, "CreateDXGIFactory2"))
        HookFunction((void*)p, (void*)Hook_CreateDXGIFactory2, (void**)&Real_CreateDXGIFactory2);
    Log("DXGI export hooks installed");
}
