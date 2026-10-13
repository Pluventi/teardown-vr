// Test-only OpenXR runtime ("mock headset") for Teardown VR.
//
// It lets the mod's real OpenXR code run without a headset: instance, system,
// D3D12 session on the game's device, swapchains, frame pacing, layers,
// actions/controllers (keyboard driven), haptics, refresh rate. Every submitted
// frame is checked, statistics are logged, and the composited layers are saved
// as BMP files on request (controls.txt dump=<n>; every 8 s with autodump=1) so
// they can be inspected.
//
// Use: [Debug] openxrRuntime=<path to tdvr_mock_openxr.json> in TeardownVR.ini.
// Output: <dll folder>\mock_out\mock.log and *.bmp. Never shipped to players.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
struct XrInstance_T { bool refreshExt = false; };
struct XrActionSet_T { std::string name; };
struct XrAction_T { std::string name; XrActionType type; XrActionSet_T* set; };
struct XrSession_T {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    // GPU readback for the BMP dumps
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
};
struct XrSpace_T {
    bool isAction = false;
    XrReferenceSpaceType ref = XR_REFERENCE_SPACE_TYPE_STAGE;
    XrAction_T* action = nullptr;
    XrPath subaction = XR_NULL_PATH;
    XrPosef offset{};
};
struct XrSwapchain_T {
    std::vector<ID3D12Resource*> images;
    uint32_t next = 0, acquired = UINT32_MAX, lastReleased = UINT32_MAX;
    int w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

namespace {

std::mutex g_mutex;
std::string g_outDir;
FILE* g_log = nullptr;
LARGE_INTEGER g_qpf;
XrTime g_t0 = 0;
float g_refresh = 72.0f;
std::vector<std::string> g_paths;
std::map<XrAction_T*, std::vector<std::string>> g_bindings;   // oculus/touch_controller only
std::deque<XrEventDataBuffer> g_events;
XrSession_T* g_session = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

XrTime Now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (XrTime)((double)c.QuadPart * 1e9 / (double)g_qpf.QuadPart);
}

double Seconds(XrTime t) { return (t - g_t0) * 1e-9; }

// --- math -------------------------------------------------------------------
XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
XrQuaternionf QConj(XrQuaternionf q) { return { -q.x, -q.y, -q.z, q.w }; }
XrVector3f QRot(XrQuaternionf q, XrVector3f v)
{
    XrQuaternionf p{ v.x, v.y, v.z, 0 };
    XrQuaternionf r = QMul(QMul(q, p), QConj(q));
    return { r.x, r.y, r.z };
}
XrQuaternionf AxisAngle(float x, float y, float z, float a)
{
    float s = std::sin(a * 0.5f);
    return { x * s, y * s, z * s, std::cos(a * 0.5f) };
}
XrVector3f Add(XrVector3f a, XrVector3f b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
XrPosef PMul(const XrPosef& a, const XrPosef& b)   // a * b (b in a's frame)
{
    return { QMul(a.orientation, b.orientation), Add(a.position, QRot(a.orientation, b.position)) };
}
XrPosef PInv(const XrPosef& a)
{
    XrQuaternionf c = QConj(a.orientation);
    XrVector3f p = QRot(c, a.position);
    return { c, { -p.x, -p.y, -p.z } };
}
const XrPosef kIdentity{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
constexpr float kDeg = 3.14159265f / 180.0f;

// --- scripted headset ---------------------------------------------------------
bool g_headStill = false;      // controls.txt "headstill=1": head fixed at (0, 1.65, 0) looking -Z
float g_aimYaw = 0, g_aimPitch = 0;
bool g_aimScripted = false;    // controls.txt "aimyaw"/"aimpitch": right hand aim, degrees, stage space
bool g_aimSway = false;        // controls.txt "aimsway=1": small hand tremor on the scripted aim
float g_handOff[3] = {};       // controls.txt "hx"/"hy"/"hz": right hand offset (m, stage space)
float g_aimRate = 0;           // controls.txt "aimrate": the scripted aim moves toward aimyaw/aimpitch at this speed (deg/s)
bool g_laimScripted = false;   // controls.txt "laimyaw"/"laimpitch": left hand aim, degrees, stage space
float g_laimYaw = 0, g_laimPitch = 0;
float g_headJitter = 0;        // controls.txt "hjit": tremor of the still head (degrees), like a real headset
float g_headYawDeg = 0, g_headPitchDeg = 0;   // controls.txt "headyaw"/"headpitch": still head turned (degrees)
int g_dumpRequest = 0;         // controls.txt "dump=<n>": a new n saves the next frame as snap_<n>_*.bmp
int g_dumpDone = 0;
// controls.txt "autodump=1": also save both eyes every 8 s (the first 40 times). Off by default: each
// save blocks xrEndFrame for 55-76 ms, which showed as fake "VR submit" hitches in the mod's log.
bool g_autoDump = false;
// controls.txt "vdhold=<ms>" and "vdholdevery=<s>": every <s> seconds xrEndFrame blocks <ms> ms, like Virtual
// Desktop waiting for its stream (Wi-Fi, the headset's decoder); "vdholdburst=<n>" holds n frames in a row
float g_vdHoldMs = 0, g_vdHoldEvery = 0, g_vdHoldBurst = 1;

XrPosef HeadPose(XrTime time)
{
    if (g_headStill) {
        double t = Seconds(time);
        float j = g_headJitter * kDeg;
        float yaw = g_headYawDeg * kDeg + j * (float)(std::sin(t * 7.3) + 0.5 * std::sin(t * 13.1));
        float pitch = g_headPitchDeg * kDeg + j * (float)(std::sin(t * 5.7) + 0.5 * std::sin(t * 11.3));
        return { QMul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, pitch)), { 0, 1.65f, 0 } };
    }
    double t = Seconds(time);
    float yaw = (float)std::sin(t * 0.4) * 25 * kDeg;
    float pitch = (float)std::sin(t * 0.27) * 8 * kDeg;
    XrPosef p;
    p.orientation = QMul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, pitch));
    p.position = { 0.25f * (float)std::sin(t * 0.3), 1.65f + 0.03f * (float)std::sin(t * 1.1), 0.15f * (float)std::sin(t * 0.21) };
    return p;
}

float HeadYaw(XrTime time) { return g_headStill ? g_headYawDeg * kDeg : (float)std::sin(Seconds(time) * 0.4) * 25 * kDeg; }

XrPosef HandPose(int hand, XrTime time)
{
    XrPosef head = HeadPose(time);
    float yaw = HeadYaw(time);
    XrQuaternionf body = AxisAngle(0, 1, 0, yaw);
    XrPosef p;
    p.position = Add(head.position, QRot(body, { hand ? 0.2f : -0.2f, -0.4f, -0.35f }));
    float sway = hand ? (float)std::sin(Seconds(time) * 0.7) * 15 * kDeg : 0;
    p.orientation = QMul(AxisAngle(0, 1, 0, yaw + sway), AxisAngle(1, 0, 0, -15 * kDeg));
    if (hand == 0 && g_laimScripted) p.orientation = QMul(AxisAngle(0, 1, 0, g_laimYaw), AxisAngle(1, 0, 0, g_laimPitch));
    if (hand == 1 && g_aimScripted) {
        float sy = g_aimSway ? (float)std::sin(Seconds(time) * 1.3) * 4 * kDeg : 0;
        float sp = g_aimSway ? (float)std::sin(Seconds(time) * 1.7) * 2 * kDeg : 0;
        static float curYaw = 0, curPitch = 0;
        static double lastT = 0;
        double tt = Seconds(time);
        float step = g_aimRate > 0 && lastT > 0 ? (float)(tt - lastT) * g_aimRate * kDeg : 1e9f;
        lastT = tt;
        curYaw += std::clamp(g_aimYaw - curYaw, -step, step);
        curPitch += std::clamp(g_aimPitch - curPitch, -step, step);
        p.orientation = QMul(AxisAngle(0, 1, 0, curYaw + sy), AxisAngle(1, 0, 0, curPitch + sp));
        p.position.x += g_handOff[0];
        p.position.y += g_handOff[1];
        p.position.z += g_handOff[2];
    }
    return p;
}

XrPosef SpaceWorld(const XrSpace_T* s, XrTime time)
{
    XrPosef base = kIdentity;
    if (s->isAction) {
        int hand = 1;
        if (s->subaction != XR_NULL_PATH && s->subaction <= g_paths.size() &&
            g_paths[s->subaction - 1] == "/user/hand/left")
            hand = 0;
        base = HandPose(hand, time);
    } else if (s->ref == XR_REFERENCE_SPACE_TYPE_VIEW) {
        base = HeadPose(time);
    } else if (s->ref == XR_REFERENCE_SPACE_TYPE_LOCAL) {
        base.position = { 0, 1.65f, 0 };
    }
    return PMul(base, s->offset);
}

// --- keyboard driven controllers ------------------------------------------------
struct Controls { float stickX[2]{}, stickY[2]{}, trigger[2]{}, squeeze[2]{}; bool a[2]{}, b[2]{}, menu[2]{}, stickClick[2]{}; };
Controls g_controls;

bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Optional scripted controls: mock_out\controls.txt, lines "name=value"
// (lx ly rx ry lt rt lg rg a b x y menu lclick rclick aimyaw aimpitch). Lets a test
// drive the controllers without keyboard focus.

void ReadControlFile(Controls& c)
{
    static FILETIME lastWrite{};
    static std::map<std::string, float> values;
    std::string file = g_outDir + "\\controls.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(file.c_str(), GetFileExInfoStandard, &fa)) { values.clear(); g_aimScripted = g_laimScripted = g_headStill = g_autoDump = false; return; }
    if (CompareFileTime(&fa.ftLastWriteTime, &lastWrite) != 0) {
        lastWrite = fa.ftLastWriteTime;
        values.clear();
        if (FILE* f = fopen(file.c_str(), "r")) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                char name[32];
                float v;
                if (sscanf(line, " %31[a-z] = %f", name, &v) == 2) values[name] = v;
            }
            fclose(f);
        }
        Log("controls: %zu values", values.size());
    }
    auto get = [&](const char* n) { auto it = values.find(n); return it == values.end() ? 0.0f : it->second; };
    c.stickX[0] += get("lx"); c.stickY[0] += get("ly");
    c.stickX[1] += get("rx"); c.stickY[1] += get("ry");
    c.trigger[0] = std::max(c.trigger[0], get("lt")); c.trigger[1] = std::max(c.trigger[1], get("rt"));
    c.squeeze[0] = std::max(c.squeeze[0], get("lg")); c.squeeze[1] = std::max(c.squeeze[1], get("rg"));
    c.a[1] = c.a[1] || get("a") > 0.5f;
    c.b[1] = c.b[1] || get("b") > 0.5f;
    c.a[0] = c.a[0] || get("x") > 0.5f;
    c.menu[0] = c.menu[0] || get("menu") > 0.5f;
    c.stickClick[0] = c.stickClick[0] || get("lclick") > 0.5f;
    c.stickClick[1] = c.stickClick[1] || get("rclick") > 0.5f;
    c.b[0] = c.b[0] || get("y") > 0.5f;
    g_aimScripted = values.count("aimyaw") || values.count("aimpitch");
    g_headStill = get("headstill") > 0.5f;
    g_dumpRequest = (int)get("dump");
    g_autoDump = get("autodump") > 0.5f;
    g_vdHoldMs = get("vdhold");
    g_vdHoldEvery = get("vdholdevery");
    g_vdHoldBurst = std::max(1.0f, get("vdholdburst"));
    g_aimSway = get("aimsway") > 0.5f;
    g_aimYaw = get("aimyaw") * kDeg;
    g_aimPitch = get("aimpitch") * kDeg;
    g_headJitter = get("hjit");
    g_headYawDeg = get("headyaw");
    g_headPitchDeg = get("headpitch");
    g_aimRate = get("aimrate");
    g_laimScripted = values.count("laimyaw") || values.count("laimpitch");
    g_laimYaw = get("laimyaw") * kDeg;
    g_laimPitch = get("laimpitch") * kDeg;
    g_handOff[0] = get("hx");
    g_handOff[1] = get("hy");
    g_handOff[2] = get("hz");
}

void ReadControls()
{
    Controls c;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) {
        c.stickY[0] = (Key('I') ? 1.f : 0.f) - (Key('K') ? 1.f : 0.f);
        c.stickX[0] = (Key('L') ? 1.f : 0.f) - (Key('J') ? 1.f : 0.f);
        c.stickX[1] = (Key('O') ? 1.f : 0.f) - (Key('U') ? 1.f : 0.f);
        c.stickY[1] = (Key('T') ? 1.f : 0.f) - (Key('G') ? 1.f : 0.f);
        c.trigger[1] = Key('H') ? 1.f : 0.f;
        c.squeeze[1] = Key('N') ? 1.f : 0.f;
        c.trigger[0] = Key('Y') ? 1.f : 0.f;
        c.a[1] = Key('M');
        c.b[1] = Key('B');
        c.a[0] = Key('V');
        c.menu[0] = Key('Z');
        c.stickClick[0] = Key('P');
    }
    ReadControlFile(c);
    g_controls = c;
}

// Value of a binding path such as "/user/hand/right/input/trigger/value".
bool ReadPath(const std::string& path, float& f, XrVector2f& v, bool& b)
{
    int hand = path.rfind("/user/hand/left", 0) == 0 ? 0 : path.rfind("/user/hand/right", 0) == 0 ? 1 : -1;
    if (hand < 0) return false;
    std::string comp = path.substr(hand == 0 ? 15 : 16);
    const Controls& c = g_controls;
    f = 0; v = { 0, 0 }; b = false;
    if (comp == "/input/thumbstick") v = { c.stickX[hand], c.stickY[hand] };
    else if (comp == "/input/trigger/value") f = c.trigger[hand];
    else if (comp == "/input/squeeze/value") f = c.squeeze[hand];
    else if (comp == "/input/a/click" || comp == "/input/x/click") b = c.a[hand];
    else if (comp == "/input/b/click" || comp == "/input/y/click") b = c.b[hand];
    else if (comp == "/input/menu/click") b = c.menu[hand];
    else if (comp == "/input/thumbstick/click") b = c.stickClick[hand];
    else return comp.find("/pose") != std::string::npos || comp.find("/output/") != std::string::npos;
    if (f > 0.5f) b = true;
    return true;
}

// --- frame statistics and dumps -------------------------------------------------
struct Stats {
    uint64_t frames = 0, projFrames = 0, quadFrames = 0, emptyFrames = 0, haptics[2]{};
    double windowStart = 0;
    uint64_t windowFrames = 0;
    XrFovf fov[2]{};
    XrPosef pose[2]{};
    int eyeW = 0, eyeH = 0;
    int problems = 0;
} g_stats;
int g_dumps = 0;

bool WriteBmp(const std::string& file, const uint8_t* data, UINT rowPitch, int w, int h, bool bgra)
{
    FILE* f = fopen(file.c_str(), "wb");
    if (!f) return false;
    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + stride * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> row(stride);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* s = data + (size_t)y * rowPitch;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = s + x * 4;
            row[x * 3 + 0] = bgra ? p[0] : p[2];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = bgra ? p[2] : p[0];
        }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

// Copies the last released image of a swapchain to the CPU and writes a BMP.
void DumpImage(XrSession_T* s, XrSwapchain_T* sc, const std::string& name)
{
    if (!sc || sc->lastReleased >= sc->images.size()) return;
    if (!s->alloc) {
        s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s->alloc));
        s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->alloc, nullptr, IID_PPV_ARGS(&s->list));
        s->list->Close();
        s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->fence));
        s->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ID3D12Resource* tex = sc->images[sc->lastReleased];
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 rowSize, total;
    s->device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    if (FAILED(s->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&buf))))
        return;
    s->alloc->Reset();
    s->list->Reset(s->alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s->list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{ buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    s->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    s->list->ResourceBarrier(1, &b);
    s->list->Close();
    ID3D12CommandList* lists[] = { s->list };
    s->queue->ExecuteCommandLists(1, lists);
    s->queue->Signal(s->fence, ++s->fenceValue);
    s->fence->SetEventOnCompletion(s->fenceValue, s->fenceEvent);
    if (WaitForSingleObject(s->fenceEvent, 3000) == WAIT_OBJECT_0) {
        void* p = nullptr;
        D3D12_RANGE r{ 0, (SIZE_T)total };
        if (SUCCEEDED(buf->Map(0, &r, &p))) {
            bool bgra = sc->format == DXGI_FORMAT_B8G8R8A8_UNORM || sc->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            std::string file = g_outDir + "\\" + name + ".bmp";
            WriteBmp(file, (const uint8_t*)p + fp.Offset, fp.Footprint.RowPitch, (int)d.Width, (int)d.Height, bgra);
            D3D12_RANGE none{ 0, 0 };
            buf->Unmap(0, &none);
            Log("dump %s (%dx%d)", file.c_str(), (int)d.Width, (int)d.Height);
        }
    } else {
        Log("dump %s: GPU timeout", name.c_str());
    }
    buf->Release();
}

void Problem(const char* fmt, ...)
{
    if (g_stats.problems++ > 40) return;
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    Log("PROBLEM: %s", b);
}

void PushState(XrSession_T* s, XrSessionState st)
{
    s->state = st;
    XrEventDataBuffer e{ XR_TYPE_EVENT_DATA_BUFFER };
    auto* c = (XrEventDataSessionStateChanged*)&e;
    c->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    c->session = s;
    c->state = st;
    c->time = Now();
    g_events.push_back(e);
}

XrResult CopyString(const char* s, uint32_t cap, uint32_t* out, char* buf)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (out) *out = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, s, n);
    return XR_SUCCESS;
}

template <class T>
XrResult Enumerate(const std::vector<T>& v, uint32_t cap, uint32_t* out, T* dst)
{
    if (out) *out = (uint32_t)v.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < v.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    for (size_t i = 0; i < v.size(); ++i) dst[i] = v[i];
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
const char* kExtensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME };

XRAPI_ATTR XrResult XRAPI_CALL EnumerateInstanceExtensionProperties(const char* layer, uint32_t cap, uint32_t* count,
                                                                    XrExtensionProperties* props)
{
    if (layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; ++i) {
        strcpy(props[i].extensionName, kExtensions[i]);
        props[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* ci, XrInstance* out)
{
    auto* inst = new XrInstance_T;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        const char* e = ci->enabledExtensionNames[i];
        if (!strcmp(e, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) inst->refreshExt = true;
        else if (strcmp(e, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) != 0) { delete inst; return XR_ERROR_EXTENSION_NOT_PRESENT; }
    }
    Log("xrCreateInstance app='%s' extensions=%u", ci->applicationInfo.applicationName, ci->enabledExtensionCount);
    *out = inst;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroyInstance(XrInstance i) { Log("xrDestroyInstance"); delete i; return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL GetInstanceProperties(XrInstance, XrInstanceProperties* p)
{
    strcpy(p->runtimeName, "TDVR Mock Headset");
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer* e)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_events.empty()) return XR_EVENT_UNAVAILABLE;
    *e = g_events.front();
    g_events.pop_front();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL ResultToString(XrInstance, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE])
{
    snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", (int)r);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL StructureTypeToString(XrInstance, XrStructureType t, char buf[XR_MAX_STRUCTURE_NAME_SIZE])
{
    snprintf(buf, XR_MAX_STRUCTURE_NAME_SIZE, "XR_TYPE_%d", (int)t);
    return XR_SUCCESS;
}

// Test of the mod's headset detection ([Debug] xrAutoRuntimes, vr.cpp): no headset when the loader loaded this DLL
// through a .json whose name has "nohmd" (tdvr_mock_openxr_nohmd.json: the same DLL; the name is read when the DLL
// is loaded, so a loader that kept the previous runtime loaded would show), or while controls.txt has "nohmd=1"
// (mockctl.py set nohmd=1; nohmd=0: the headset "connects").
std::string g_loadJson;   // XR_RUNTIME_JSON when this DLL was loaded

const char* NoHeadset()
{
    std::string lower = g_loadJson;
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (lower.find("nohmd") != std::string::npos) return "loaded through a nohmd .json";
    bool off = false;
    if (FILE* f = fopen((g_outDir + "\\controls.txt").c_str(), "r")) {
        char line[128];
        float v;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, " nohmd = %f", &v) == 1) off = v > 0.5f;
        fclose(f);
    }
    return off ? "controls.txt nohmd=1" : nullptr;
}

XRAPI_ATTR XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo* gi, XrSystemId* id)
{
    if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    if (const char* why = NoHeadset()) {
        Log("xrGetSystem: no headset (test: %s)", why);
        return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    *id = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p)
{
    p->systemId = 1;
    p->vendorId = 0x2833;
    strcpy(p->systemName, "Mock Quest 3 (test)");
    p->graphicsProperties.maxSwapchainImageWidth = 8192;
    p->graphicsProperties.maxSwapchainImageHeight = 8192;
    p->graphicsProperties.maxLayerCount = 16;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap,
                                                              uint32_t* n, XrEnvironmentBlendMode* m)
{
    return Enumerate(std::vector<XrEnvironmentBlendMode>{ XR_ENVIRONMENT_BLEND_MODE_OPAQUE }, cap, n, m);
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateViewConfigurations(XrInstance, XrSystemId, uint32_t cap, uint32_t* n,
                                                           XrViewConfigurationType* t)
{
    return Enumerate(std::vector<XrViewConfigurationType>{ XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO }, cap, n, t);
}

XRAPI_ATTR XrResult XRAPI_CALL GetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType t,
                                                              XrViewConfigurationProperties* p)
{
    p->viewConfigurationType = t;
    p->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap,
                                                               uint32_t* n, XrViewConfigurationView* v)
{
    *n = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; ++i) {
        v[i].recommendedImageRectWidth = 2064;
        v[i].recommendedImageRectHeight = 2208;
        v[i].maxImageRectWidth = 8192;
        v[i].maxImageRectHeight = 8192;
        v[i].recommendedSwapchainSampleCount = 1;
        v[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetD3D12GraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* r)
{
    IDXGIFactory1* f = nullptr;
    IDXGIAdapter1* a = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumAdapters1(0, &a))) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        r->adapterLuid = d.AdapterLuid;
        a->Release();
    }
    if (f) f->Release();
    r->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* out)
{
    const XrBaseInStructure* n = (const XrBaseInStructure*)ci->next;
    while (n && n->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) n = n->next;
    if (!n) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto* b = (const XrGraphicsBindingD3D12KHR*)n;
    if (!b->device || !b->queue) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto* s = new XrSession_T;
    s->device = b->device;
    s->queue = b->queue;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session = s;
    g_t0 = Now();
    PushState(s, XR_SESSION_STATE_IDLE);
    PushState(s, XR_SESSION_STATE_READY);
    Log("xrCreateSession on the game's D3D12 device %p / queue %p", b->device, b->queue);
    *out = s;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroySession(XrSession s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Log("xrDestroySession");
    if (s->list) s->list->Release();
    if (s->alloc) s->alloc->Release();
    if (s->fence) s->fence->Release();
    if (s->fenceEvent) CloseHandle(s->fenceEvent);
    if (g_session == s) g_session = nullptr;
    delete s;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL BeginSession(XrSession s, const XrSessionBeginInfo* bi)
{
    if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (s->state != XR_SESSION_STATE_READY) { Problem("xrBeginSession in state %d", s->state); return XR_ERROR_SESSION_NOT_READY; }
    s->running = true;
    PushState(s, XR_SESSION_STATE_SYNCHRONIZED);
    PushState(s, XR_SESSION_STATE_VISIBLE);
    PushState(s, XR_SESSION_STATE_FOCUSED);
    Log("xrBeginSession");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EndSession(XrSession s)
{
    s->running = false;
    Log("xrEndSession");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL RequestExitSession(XrSession s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    PushState(s, XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateReferenceSpaces(XrSession, uint32_t cap, uint32_t* n, XrReferenceSpaceType* t)
{
    return Enumerate(std::vector<XrReferenceSpaceType>{ XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                       XR_REFERENCE_SPACE_TYPE_STAGE },
                     cap, n, t);
}

XRAPI_ATTR XrResult XRAPI_CALL CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* ci, XrSpace* out)
{
    auto* s = new XrSpace_T;
    s->ref = ci->referenceSpaceType;
    s->offset = ci->poseInReferenceSpace;
    *out = s;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType, XrExtent2Df* b)
{
    *b = { 3, 3 };
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL CreateActionSpace(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* out)
{
    auto* s = new XrSpace_T;
    s->isAction = true;
    s->action = ci->action;
    s->subaction = ci->subactionPath;
    s->offset = ci->poseInActionSpace;
    *out = s;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroySpace(XrSpace s) { delete s; return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* loc)
{
    XrPosef rel = PMul(PInv(SpaceWorld(base, time)), SpaceWorld(space, time));
    loc->pose = rel;
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL LocateViews(XrSession, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* n,
                                           XrView* views)
{
    *n = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    XrPosef head = HeadPose(li->displayTime);
    XrPosef base = PInv(SpaceWorld(li->space, li->displayTime));
    const XrFovf fov[2] = { { -0.94f, 0.77f, 0.84f, -0.87f }, { -0.77f, 0.94f, 0.84f, -0.87f } };
    for (int i = 0; i < 2; ++i) {
        XrPosef eye{ { 0, 0, 0, 1 }, { i == 0 ? -0.032f : 0.032f, 0, 0 } };
        views[i].pose = PMul(base, PMul(head, eye));
        views[i].fov = fov[i];
    }
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                         XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* n, int64_t* f)
{
    return Enumerate(std::vector<int64_t>{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                          DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM },
                     cap, n, f);
}

XRAPI_ATTR XrResult XRAPI_CALL CreateSwapchain(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    auto* sc = new XrSwapchain_T;
    sc->w = (int)ci->width;
    sc->h = (int)ci->height;
    sc->format = (DXGI_FORMAT)ci->format;
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = ci->width;
    d.Height = ci->height;
    d.DepthOrArraySize = (UINT16)ci->arraySize;
    d.MipLevels = (UINT16)ci->mipCount;
    d.Format = sc->format;
    d.SampleDesc.Count = ci->sampleCount;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < 3; ++i) {
        ID3D12Resource* r = nullptr;
        HRESULT hr = s->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr)) {
            Problem("swapchain image %ux%u fmt %d: 0x%08x", ci->width, ci->height, (int)ci->format, hr);
            for (auto* img : sc->images) img->Release();
            delete sc;
            return XR_ERROR_RUNTIME_FAILURE;
        }
        sc->images.push_back(r);
    }
    Log("xrCreateSwapchain %ux%u format %d", ci->width, ci->height, (int)ci->format);
    *out = sc;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroySwapchain(XrSwapchain sc)
{
    for (auto* i : sc->images) i->Release();
    delete sc;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL EnumerateSwapchainImages(XrSwapchain sc, uint32_t cap, uint32_t* n, XrSwapchainImageBaseHeader* imgs)
{
    *n = (uint32_t)sc->images.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < *n) return XR_ERROR_SIZE_INSUFFICIENT;
    auto* d = (XrSwapchainImageD3D12KHR*)imgs;
    for (uint32_t i = 0; i < *n; ++i) {
        if (d[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) Problem("swapchain image struct type %d", d[i].type);
        d[i].texture = sc->images[i];
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo*, uint32_t* idx)
{
    if (sc->acquired != UINT32_MAX) { Problem("acquire twice without release"); return XR_ERROR_CALL_ORDER_INVALID; }
    sc->acquired = sc->next;
    sc->next = (sc->next + 1) % sc->images.size();
    *idx = sc->acquired;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL WaitSwapchainImage(XrSwapchain sc, const XrSwapchainImageWaitInfo*)
{
    return sc->acquired == UINT32_MAX ? XR_ERROR_CALL_ORDER_INVALID : XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo*)
{
    if (sc->acquired == UINT32_MAX) { Problem("release without acquire"); return XR_ERROR_CALL_ORDER_INVALID; }
    sc->lastReleased = sc->acquired;
    sc->acquired = UINT32_MAX;
    return XR_SUCCESS;
}

XrTime g_nextVsync = 0;

XRAPI_ATTR XrResult XRAPI_CALL WaitFrame(XrSession s, const XrFrameWaitInfo*, XrFrameState* fs)
{
    XrTime period = (XrTime)(1e9 / g_refresh);
    XrTime t = Now();
    if (g_nextVsync == 0 || t > g_nextVsync + period) g_nextVsync = t + period;
    // Waits on a high-resolution waitable timer (Windows 10 1803+: wakes ~0.1-0.5 ms late at
    // most). The old spin of the last 2 ms (YieldProcessor) burned up to 2 ms of the game's main
    // thread per WaitFrame, inflating its CPU in the measurements; it is kept only as a fallback
    // when such a timer cannot be created.
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while ((t = Now()) < g_nextVsync) {
        XrTime left = g_nextVsync - t;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left + 99) / 100);   // (relative, 100 ns units)
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 1000);
                continue;
            }
        }
        if (left > 2000000) Sleep((DWORD)(left / 1000000 - 1));
        else YieldProcessor();
    }
    fs->predictedDisplayTime = g_nextVsync + period;
    fs->predictedDisplayPeriod = period;
    fs->shouldRender = s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED;
    g_nextVsync += period;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }

bool ValidFov(const XrFovf& f)
{
    return f.angleLeft < f.angleRight && f.angleDown < f.angleUp && f.angleLeft > -1.6f && f.angleRight < 1.6f &&
           f.angleUp < 1.6f && f.angleDown > -1.6f;
}

XRAPI_ATTR XrResult XRAPI_CALL EndFrame(XrSession s, const XrFrameEndInfo* ei)
{
    {
        // Simulated stream hold (controls.txt vdhold / vdholdevery / vdholdburst), outside the lock like
        // VDXR's wait for its submission thread.
        static double nextHold = 0;
        static int burstLeft = 0;
        const double t = Seconds(Now());
        if (g_vdHoldMs > 0 && g_vdHoldEvery > 0) {
            if (nextHold == 0) nextHold = t + g_vdHoldEvery;
            if (burstLeft == 0 && t >= nextHold) { burstLeft = (int)g_vdHoldBurst; nextHold = t + g_vdHoldEvery; }
            if (burstLeft > 0) { --burstLeft; Sleep((DWORD)g_vdHoldMs); }
        } else {
            nextHold = 0;
            burstLeft = 0;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Stats& st = g_stats;
    st.frames++;
    st.windowFrames++;
    const XrCompositionLayerProjection* proj = nullptr;
    std::vector<const XrCompositionLayerQuad*> quads;
    for (uint32_t i = 0; i < ei->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = ei->layers[i];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) proj = (const XrCompositionLayerProjection*)l;
        else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) quads.push_back((const XrCompositionLayerQuad*)l);
        else Problem("unknown layer type %d", l->type);
    }
    if (ei->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) Problem("blend mode %d", ei->environmentBlendMode);
    if (ei->layerCount == 0) st.emptyFrames++;
    if (proj) {
        st.projFrames++;
        if (proj->viewCount != 2) Problem("projection with %u views", proj->viewCount);
        for (uint32_t v = 0; v < proj->viewCount && v < 2; ++v) {
            const XrCompositionLayerProjectionView& pv = proj->views[v];
            auto* sc = (XrSwapchain_T*)pv.subImage.swapchain;
            if (!sc) { Problem("projection view %u without swapchain", v); continue; }
            if (sc->lastReleased == UINT32_MAX) Problem("projection view %u: swapchain never released", v);
            if (pv.subImage.imageRect.extent.width > sc->w || pv.subImage.imageRect.extent.height > sc->h)
                Problem("projection view %u rect larger than swapchain", v);
            if (!ValidFov(pv.fov)) Problem("projection view %u fov %.2f %.2f %.2f %.2f", v, pv.fov.angleLeft,
                                           pv.fov.angleRight, pv.fov.angleUp, pv.fov.angleDown);
            const XrQuaternionf& q = pv.pose.orientation;
            float len = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
            if (std::abs(len - 1) > 0.01f) Problem("projection view %u orientation not normalised (%.3f)", v, len);
            st.fov[v] = pv.fov;
            st.pose[v] = pv.pose;
            st.eyeW = sc->w;
            st.eyeH = sc->h;
        }
        if (proj->viewCount == 2 && proj->views[0].subImage.swapchain == proj->views[1].subImage.swapchain &&
            proj->views[0].subImage.imageArrayIndex == proj->views[1].subImage.imageArrayIndex)
            Problem("both eyes use the same image");
    }
    if (!quads.empty()) st.quadFrames++;

    double now = Seconds(Now());
    if (now - st.windowStart >= 5.0) {
        double fps = st.windowFrames / (now - st.windowStart);
        XrVector3f d{ st.pose[1].position.x - st.pose[0].position.x, st.pose[1].position.y - st.pose[0].position.y,
                      st.pose[1].position.z - st.pose[0].position.z };
        Log("frames=%llu (%.1f/s at %.0f Hz) projection=%llu quad=%llu empty=%llu eye=%dx%d ipd=%.4f "
            "fovL=[%.2f %.2f %.2f %.2f] fovR=[%.2f %.2f %.2f %.2f] haptics L=%llu R=%llu",
            st.frames, fps, g_refresh, st.projFrames, st.quadFrames, st.emptyFrames, st.eyeW, st.eyeH,
            std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), st.fov[0].angleLeft, st.fov[0].angleRight, st.fov[0].angleUp,
            st.fov[0].angleDown, st.fov[1].angleLeft, st.fov[1].angleRight, st.fov[1].angleUp, st.fov[1].angleDown,
            st.haptics[0], st.haptics[1]);
        st.windowStart = now;
        st.windowFrames = 0;
    }

    // Save what the headset would show: on request (controls.txt dump=<n>), and with autodump=1
    // every 8 s (both eyes or the quads).
    static double nextDump = 12.0;
    bool requested = g_dumpRequest > 0 && g_dumpRequest != g_dumpDone;
    if ((g_autoDump && now >= nextDump && g_dumps < 40) || requested) {
        nextDump = now + 8.0;
        char name[64];
        int k = requested ? 100 + g_dumpRequest : g_dumps++;
        g_dumpDone = g_dumpRequest;
        if (proj && proj->viewCount == 2) {
            snprintf(name, sizeof(name), "%02d_eye_left", k);
            DumpImage(s, (XrSwapchain_T*)proj->views[0].subImage.swapchain, name);
            snprintf(name, sizeof(name), "%02d_eye_right", k);
            DumpImage(s, (XrSwapchain_T*)proj->views[1].subImage.swapchain, name);
        }
        for (size_t i = 0; i < quads.size(); ++i) {
            snprintf(name, sizeof(name), "%02d_quad%zu", k, i);
            DumpImage(s, (XrSwapchain_T*)quads[i]->subImage.swapchain, name);
            Log("  quad%zu size %.2fx%.2f m at (%.2f %.2f %.2f)", i, quads[i]->size.width, quads[i]->size.height,
                quads[i]->pose.position.x, quads[i]->pose.position.y, quads[i]->pose.position.z);
        }
    }
    return XR_SUCCESS;
}

// --- actions --------------------------------------------------------------------
XRAPI_ATTR XrResult XRAPI_CALL StringToPath(XrInstance, const char* s, XrPath* p)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = std::find(g_paths.begin(), g_paths.end(), std::string(s));
    if (it == g_paths.end()) { g_paths.push_back(s); *p = g_paths.size(); }
    else *p = (XrPath)(it - g_paths.begin()) + 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL PathToString(XrInstance, XrPath p, uint32_t cap, uint32_t* n, char* buf)
{
    if (p == XR_NULL_PATH || p > g_paths.size()) return XR_ERROR_PATH_INVALID;
    return CopyString(g_paths[p - 1].c_str(), cap, n, buf);
}

XRAPI_ATTR XrResult XRAPI_CALL CreateActionSet(XrInstance, const XrActionSetCreateInfo* ci, XrActionSet* out)
{
    *out = new XrActionSet_T{ ci->actionSetName };
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroyActionSet(XrActionSet s) { delete s; return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL CreateAction(XrActionSet set, const XrActionCreateInfo* ci, XrAction* out)
{
    *out = new XrAction_T{ ci->actionName, ci->actionType, set };
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroyAction(XrAction a) { delete a; return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL SuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding* sb)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string profile = g_paths[sb->interactionProfile - 1];
    Log("bindings suggested for %s (%u)", profile.c_str(), sb->countSuggestedBindings);
    if (profile != "/interaction_profiles/oculus/touch_controller") return XR_SUCCESS;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; ++i) {
        XrPath b = sb->suggestedBindings[i].binding;
        if (b == XR_NULL_PATH || b > g_paths.size()) return XR_ERROR_PATH_INVALID;
        float f;
        XrVector2f v;
        bool bo;
        if (!ReadPath(g_paths[b - 1], f, v, bo)) Problem("unsupported binding %s", g_paths[b - 1].c_str());
        g_bindings[sb->suggestedBindings[i].action].push_back(g_paths[b - 1]);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL AttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo* ai)
{
    Log("action sets attached (%u)", ai->countActionSets);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetCurrentInteractionProfile(XrSession, XrPath, XrInteractionProfileState* st)
{
    XrPath p;
    StringToPath(nullptr, "/interaction_profiles/oculus/touch_controller", &p);
    st->interactionProfile = p;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL SyncActions(XrSession, const XrActionsSyncInfo*)
{
    ReadControls();
    return XR_SUCCESS;
}

// Reads every binding of an action that belongs to the subaction path.
void ReadAction(const XrActionStateGetInfo* gi, float& f, XrVector2f& v, bool& b, bool& active)
{
    f = 0; v = { 0, 0 }; b = false; active = false;
    std::string sub = gi->subactionPath != XR_NULL_PATH && gi->subactionPath <= g_paths.size() ? g_paths[gi->subactionPath - 1] : "";
    auto it = g_bindings.find(gi->action);
    if (it == g_bindings.end()) return;
    for (const std::string& path : it->second) {
        if (!sub.empty() && path.rfind(sub, 0) != 0) continue;
        float pf;
        XrVector2f pv;
        bool pb;
        if (!ReadPath(path, pf, pv, pb)) continue;
        active = true;
        f = std::max(f, pf);
        if (std::abs(pv.x) + std::abs(pv.y) > std::abs(v.x) + std::abs(v.y)) v = pv;
        b = b || pb;
    }
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStateBoolean(XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* s)
{
    float f; XrVector2f v; bool b, a;
    ReadAction(gi, f, v, b, a);
    s->currentState = b;
    s->isActive = a;
    s->changedSinceLastSync = XR_FALSE;
    s->lastChangeTime = Now();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStateFloat(XrSession, const XrActionStateGetInfo* gi, XrActionStateFloat* s)
{
    float f; XrVector2f v; bool b, a;
    ReadAction(gi, f, v, b, a);
    s->currentState = f;
    s->isActive = a;
    s->changedSinceLastSync = XR_FALSE;
    s->lastChangeTime = Now();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStateVector2f(XrSession, const XrActionStateGetInfo* gi, XrActionStateVector2f* s)
{
    float f; XrVector2f v; bool b, a;
    ReadAction(gi, f, v, b, a);
    s->currentState = v;
    s->isActive = a;
    s->changedSinceLastSync = XR_FALSE;
    s->lastChangeTime = Now();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL GetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* s)
{
    s->isActive = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL ApplyHapticFeedback(XrSession, const XrHapticActionInfo* hi, const XrHapticBaseHeader* h)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    int hand = hi->subactionPath != XR_NULL_PATH && hi->subactionPath <= g_paths.size() &&
                       g_paths[hi->subactionPath - 1] == "/user/hand/left" ? 0 : 1;
    if (h->type != XR_TYPE_HAPTIC_VIBRATION) Problem("haptic struct type %d", h->type);
    if (g_stats.haptics[hand]++ < 5) {
        auto* v = (const XrHapticVibration*)h;
        Log("haptic %s amplitude %.2f duration %.3f s", hand ? "right" : "left", v->amplitude, v->duration * 1e-9);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL StopHapticFeedback(XrSession, const XrHapticActionInfo*) { return XR_SUCCESS; }

// --- XR_FB_display_refresh_rate ------------------------------------------------------
XRAPI_ATTR XrResult XRAPI_CALL EnumerateDisplayRefreshRatesFB(XrSession, uint32_t cap, uint32_t* n, float* r)
{
    return Enumerate(std::vector<float>{ 72, 80, 90, 120 }, cap, n, r);
}

XRAPI_ATTR XrResult XRAPI_CALL GetDisplayRefreshRateFB(XrSession, float* r)
{
    *r = g_refresh;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL RequestDisplayRefreshRateFB(XrSession, float r)
{
    if (r != 0 && r != 72 && r != 80 && r != 90 && r != 120) return XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
    g_refresh = r == 0 ? 72.0f : r;
    Log("refresh rate set to %.0f Hz", g_refresh);
    return XR_SUCCESS;
}

// --- dispatch ---------------------------------------------------------------------------
XRAPI_ATTR XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn);

struct Entry { const char* name; PFN_xrVoidFunction fn; };
#define E(n) { "xr" #n, (PFN_xrVoidFunction)n }
const Entry kEntries[] = {
    E(GetInstanceProcAddr), E(EnumerateInstanceExtensionProperties), E(CreateInstance), E(DestroyInstance),
    E(GetInstanceProperties), E(PollEvent), E(ResultToString), E(StructureTypeToString), E(GetSystem),
    E(GetSystemProperties), E(EnumerateEnvironmentBlendModes), E(EnumerateViewConfigurations),
    E(GetViewConfigurationProperties), E(EnumerateViewConfigurationViews), E(GetD3D12GraphicsRequirementsKHR),
    E(CreateSession), E(DestroySession), E(BeginSession), E(EndSession), E(RequestExitSession),
    E(EnumerateReferenceSpaces), E(CreateReferenceSpace), E(GetReferenceSpaceBoundsRect), E(CreateActionSpace),
    E(DestroySpace), E(LocateSpace), E(LocateViews), E(EnumerateSwapchainFormats), E(CreateSwapchain),
    E(DestroySwapchain), E(EnumerateSwapchainImages), E(AcquireSwapchainImage), E(WaitSwapchainImage),
    E(ReleaseSwapchainImage), E(WaitFrame), E(BeginFrame), E(EndFrame), E(StringToPath), E(PathToString),
    E(CreateActionSet), E(DestroyActionSet), E(CreateAction), E(DestroyAction), E(SuggestInteractionProfileBindings),
    E(AttachSessionActionSets), E(GetCurrentInteractionProfile), E(SyncActions), E(GetActionStateBoolean),
    E(GetActionStateFloat), E(GetActionStateVector2f), E(GetActionStatePose), E(ApplyHapticFeedback),
    E(StopHapticFeedback), E(EnumerateDisplayRefreshRatesFB), E(GetDisplayRefreshRateFB), E(RequestDisplayRefreshRateFB),
};
#undef E

XRAPI_ATTR XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn)
{
    *fn = nullptr;
    for (const Entry& e : kEntries)
        if (!strcmp(e.name, name)) {
            bool global = !strcmp(name, "xrEnumerateInstanceExtensionProperties") || !strcmp(name, "xrCreateInstance") ||
                          !strcmp(name, "xrGetInstanceProcAddr");
            if (!inst && !global) return XR_ERROR_HANDLE_INVALID;
            *fn = e.fn;
            return XR_SUCCESS;
        }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

} // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* li,
                                                                                       XrNegotiateRuntimeRequest* rr)
{
    if (!li || !rr || li->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        rr->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (li->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION) return XR_ERROR_INITIALIZATION_FAILED;
    rr->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    rr->runtimeApiVersion = XR_CURRENT_API_VERSION;
    rr->getInstanceProcAddr = GetInstanceProcAddr;
    return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        QueryPerformanceFrequency(&g_qpf);
        g_t0 = Now();
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, MAX_PATH);
        g_outDir = path;
        g_outDir = g_outDir.substr(0, g_outDir.find_last_of("\\/")) + "\\mock_out";
        CreateDirectoryA(g_outDir.c_str(), nullptr);
        // (the mod's headset detection loads and unloads this DLL several times in one game process: the log is
        // only started anew at the process's first load)
        const bool again = GetEnvironmentVariableA("TDVR_MOCK_LOADED", nullptr, 0) > 0;
        SetEnvironmentVariableA("TDVR_MOCK_LOADED", "1");
        g_log = fopen((g_outDir + "\\mock.log").c_str(), again ? "a" : "w");
        char json[MAX_PATH] = "";
        GetEnvironmentVariableA("XR_RUNTIME_JSON", json, sizeof(json));
        g_loadJson = json;
        Log("TDVR mock OpenXR runtime loaded%s%s", json[0] ? " through " : "", json);
    }
    return TRUE;
}
