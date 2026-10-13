// OpenXR session + frame orchestration.
//
// Stereo is produced by "synchronised alternate eye rendering": the Lua mod
// asks us for a camera every game frame; we hand out the left eye, then the
// right eye, from a single set of predicted poses. Each game frame is copied
// into the matching eye swapchain and the pair is submitted together, so the
// headset gets one stereo frame for every two game frames.
#include "vr.h"
#include "bridge.h"
#include "config.h"
#include "d3d_hooks.h"
#include "frameb.h"
#include "input.h"
#include "log.h"
#include "perf.h"
#include "prof.h"
#include "sampler.h"
#include "engine.h"
#include "gpu_trace.h"
#include "gpu_watch.h"
#include "i18n.h"
#include "inventory.h"
#include "re_tools.h"
#include "render.h"
#include "ui.h"

#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    int w = 0, h = 0;
    std::vector<ID3D12Resource*> images;
    bool acquired = false;
};

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_appSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
bool g_stage = false;
double g_lastLuaRequest = 0;
float g_screenPointerU = -1, g_screenPointerV = -1;   // controller pointer on the virtual screen (0..1)   // time of the last request from the Lua mod
float g_screenOtherU = -1, g_screenOtherV = -1;       // the other hand's ray on it (offHandPointer: a grey mark), -1 = none
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
DXGI_FORMAT g_colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
XrViewConfigurationView g_viewCfg[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
Swapchain g_eyeSc[2], g_screenSc, g_uiSc;
double g_nextRetry = 0;

PFN_xrGetD3D12GraphicsRequirementsKHR pGetD3D12Req = nullptr;
PFN_xrEnumerateDisplayRefreshRatesFB pEnumRates = nullptr;
PFN_xrRequestDisplayRefreshRateFB pRequestRate = nullptr;
PFN_xrGetDisplayRefreshRateFB pGetRate = nullptr;
int g_appliedRefresh = -1;

// Hidden-area fill ([Performance] hiddenAreaFill; FillHidden, below). The runtime's lens mask query
// (XR_KHR_visibility_mask; nullptr: the runtime does not offer it).
PFN_xrGetVisibilityMaskKHR pGetVisMask = nullptr;
struct HiddenArea
{
    bool fetched = false;               // the runtime's masks were asked for (this session, or since it changed them)
    unsigned gen = 0;                   // ... how many times (a new answer: the rectangles are built again)
    std::vector<XrVector2f> tri[2];     // hidden triangles per eye, 3 vertices each: view plane z = -1, +x right, +y up
    // Per eye: what its rectangles were built from, and them
    bool built[2] = {};
    int w[2] = {}, h[2] = {}, mode[2] = {};
    unsigned builtGen[2] = {};
    float guard[2] = {};
    XrFovf fov[2] = {}, view[2] = {};
    std::vector<D3D12_RECT> rects[2];
    double fillPct[2] = {};             // share of the image they cover (percent)
    double pctSum = 0;                  // since the last "Stereo frames" line: shares cleared, summed...
    int images = 0;                     // ... over this many eye images
    double lastBuild[2] = {};           // Now() of the last build, and builds less than 2 s apart in a row
    int quick[2] = {};
    bool off = false;                   // stopped for this session (a field of view that keeps changing)
    int loggedMode = -1;
    int builds = 0;                     // build lines logged
};
HiddenArea g_hidden;

// Actions
XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction aAim, aGrip, aStick, aTrigger, aSqueeze, aStickClick, aPrimary, aSecondary, aMenu, aHaptic;
XrPath g_handPath[2];
XrSpace g_aimSpace[2] = {}, g_gripSpace[2] = {};

// Frame state (guarded by g_mutex: the Lua thread reads it)
std::mutex g_mutex;
bool g_frameBegun = false;
XrFrameState g_frameState = { XR_TYPE_FRAME_STATE };
XrView g_views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
bool g_viewsValid = false;
bool g_leftDone = false;
Pose g_headRaw;                 // app space
HandState g_handsRaw[2];        // app space
Pose g_recenter;                // app space pose of the recentered origin (yaw only)
float g_refHeadY = 1.65f;
bool g_recenterPending = true;
int g_recenterCount = 0;

bool g_requestedThisFrame = false;
bool g_requestActive = false;
int g_requestEye = 0;
Pose g_renderPose[2];
XrFovf g_renderFov[2];
// The field of view each eye image covers, handed to the runtime (pv.fov): with fovCrop the headset's whole field
// (the game draws the covered part of it, g_renderFov - CropRect), without it g_renderFov.
XrFovf g_submitFov[2];
// fovCrop: the share of the field (each way) the game's back buffer was sized for at start (StartCoverage); the eye
// images keep the whole field at its pixel density (back buffer / g_cropBase). 1 = not sized by us (PC screen).
float g_cropBase = 1;
int g_bbW = 1920, g_bbH = 1080;

XrCompositionLayerProjectionView g_projViews[2] = {
    { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };

// UI / screen quads (app space)
Pose g_uiPose;
Pose g_invPose;                 // VR inventory panel (see inventory.h, UpdateInventoryPanel)
bool g_uiPlacePending = true;
bool g_panelSnap = false;       // recentred: the VR inventory goes back in front of the body (UpdatePanels)
float g_hapticReq[2] = { 0, 0 };
float g_luaVignette = 0;
bool g_mapView = false;          // the level map is shown in 3D this frame

VrStatus g_status;

double Now()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

const char* ResultStr(XrResult r)
{
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance && xrResultToString(g_instance, r, buf) == XR_SUCCESS) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", r);
    return buf;
}

#define XR_LOG_FAIL(call)                                                       \
    ([&]() {                                                                    \
        XrResult _r = (call);                                                   \
        if (XR_FAILED(_r)) Log("%s failed: %s", #call, ResultStr(_r));          \
        return _r;                                                              \
    }())

Pose ToPose(const XrPosef& p)
{
    return { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }, { p.position.x, p.position.y, p.position.z } };
}
XrPosef ToXr(const Pose& p)
{
    XrPosef r;
    r.orientation = { p.q.x, p.q.y, p.q.z, p.q.w };
    r.position = { p.p.x, p.p.y, p.p.z };
    return r;
}

// ---------------------------------------------------------------------------
// Instance / session
// ---------------------------------------------------------------------------
void DestroySwapchain(Swapchain& sc)
{
    if (sc.handle) xrDestroySwapchain(sc.handle);
    sc = Swapchain{};
}

void DestroySession()
{
    Render::WaitIdle();
    DestroySwapchain(g_eyeSc[0]);
    DestroySwapchain(g_eyeSc[1]);
    DestroySwapchain(g_screenSc);
    DestroySwapchain(g_uiSc);
    for (int i = 0; i < 2; ++i) {
        if (g_aimSpace[i]) xrDestroySpace(g_aimSpace[i]);
        if (g_gripSpace[i]) xrDestroySpace(g_gripSpace[i]);
        g_aimSpace[i] = g_gripSpace[i] = XR_NULL_HANDLE;
    }
    if (g_actionSet) xrDestroyActionSet(g_actionSet);
    g_actionSet = XR_NULL_HANDLE;
    if (g_appSpace) xrDestroySpace(g_appSpace);
    if (g_viewSpace) xrDestroySpace(g_viewSpace);
    g_appSpace = g_viewSpace = XR_NULL_HANDLE;
    if (g_session) xrDestroySession(g_session);
    g_session = XR_NULL_HANDLE;
    g_running = false;
    g_frameBegun = false;
    g_leftDone = false;
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_appliedRefresh = -1;
    g_status.sessionRunning = false;
    g_hidden = HiddenArea{};   // (the masks and rectangles belong to the session)
}

void DestroyInstance()
{
    DestroySession();
    if (g_instance) xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
    g_system = XR_NULL_SYSTEM_ID;
}

bool CreateInstance()
{
    uint32_t count = 0;
    if (XrResult er = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr); XR_FAILED(er)) {
        static int logged = 0;
        if (logged++ < 3) Log("XR: no OpenXR runtime (xrEnumerateInstanceExtensionProperties %d)", er);
        g_status.state = { "No OpenXR runtime (install Virtual Desktop / Meta Link / SteamVR)",
                           "Aucun runtime OpenXR (installe Virtual Desktop / Meta Link / SteamVR)" };
        return false;
    }
    std::vector<XrExtensionProperties> exts(count, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    bool hasD3D12 = false, hasRefresh = false, hasVisMask = false;
    for (auto& e : exts) {
        if (!strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME)) hasD3D12 = true;
        if (!strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) hasRefresh = true;
        if (!strcmp(e.extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME)) hasVisMask = true;
    }
    if (!hasD3D12) {
        g_status.state = { "The OpenXR runtime does not support Direct3D 12",
                           "Le runtime OpenXR ne supporte pas Direct3D 12" };
        Log("XR: runtime lacks XR_KHR_D3D12_enable");
        return false;
    }
    std::vector<const char*> enable = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
    if (hasRefresh) enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    // The lens mask query (hidden-area fill): only when hiddenAreaFill is set in the ini, or the control
    // file TeardownVR_perf.txt has a "hiddenAreaFill=N" line when the instance is made (the file is read
    // at engine start, frameb.h) - otherwise the instance is created exactly as before. It only adds a
    // query; the mask is asked for with hiddenAreaFill 1 or 3 (FillHidden).
    hasVisMask = hasVisMask && (Cfg().hiddenAreaFill != 0 || FrameB::FileHiddenArea() >= 0);
    if (hasVisMask) enable.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);

    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "Teardown VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "TeardownVR");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = (uint32_t)enable.size();
    ci.enabledExtensionNames = enable.data();
    XrResult r = xrCreateInstance(&ci, &g_instance);
    if (XR_FAILED(r)) {
        Log("XR: xrCreateInstance failed (%d)", r);
        g_status.state = { "Could not start OpenXR", "Impossible de démarrer OpenXR" };
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(g_instance, &props);
    char rt[160];
    snprintf(rt, sizeof(rt), "%s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
             XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
    g_status.runtime = rt;
    Log("XR: instance created, runtime: %s", rt);

    xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pGetD3D12Req);
    pGetVisMask = nullptr;
    if (hasVisMask) xrGetInstanceProcAddr(g_instance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction*)&pGetVisMask);
    Log("XR: lens mask (XR_KHR_visibility_mask) %s", pGetVisMask ? "available" : "not asked for, or not offered by the runtime");
    // (reset first: after a switch to another runtime - headset detection, RuntimeRedetect - the previous
    // runtime is unloaded, and the new one may not offer the extension)
    pEnumRates = nullptr;
    pRequestRate = nullptr;
    pGetRate = nullptr;
    if (hasRefresh) {
        xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&pEnumRates);
        xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&pRequestRate);
        xrGetInstanceProcAddr(g_instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&pGetRate);
    }
    return true;
}

bool GetSystem()
{
    XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_instance, &gi, &g_system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        g_status.state = { "Headset not detected (connect the Quest: Virtual Desktop or Quest Link)",
                           "Casque non détecté (connecte le Quest : Virtual Desktop ou Quest Link)" };
        return false;
    }
    if (XR_FAILED(r)) {
        Log("XR: xrGetSystem failed: %s", ResultStr(r));
        g_status.state = { "OpenXR error (xrGetSystem)", "Erreur OpenXR (xrGetSystem)" };
        if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_RUNTIME_FAILURE) DestroyInstance();
        return false;
    }
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(g_instance, g_system, &sp);
    g_status.system = sp.systemName;
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, g_viewCfg);
    g_status.recommendedW = (int)g_viewCfg[0].recommendedImageRectWidth;
    g_status.recommendedH = (int)g_viewCfg[0].recommendedImageRectHeight;
    Log("XR: system '%s', recommended %ux%u", sp.systemName, g_viewCfg[0].recommendedImageRectWidth,
        g_viewCfg[0].recommendedImageRectHeight);
    return true;
}

XrAction MakeAction(const char* name, const char* loc, XrActionType type)
{
    XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
    strcpy(ai.actionName, name);
    strcpy(ai.localizedActionName, loc);
    ai.actionType = type;
    ai.countSubactionPaths = 2;
    ai.subactionPaths = g_handPath;
    XrAction a = XR_NULL_HANDLE;
    XR_LOG_FAIL(xrCreateAction(g_actionSet, &ai, &a));
    return a;
}

XrPath P(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, s, &p);
    return p;
}

void SuggestBindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& list)
{
    std::vector<XrActionSuggestedBinding> b;
    for (auto& [a, path] : list) b.push_back({ a, P(path) });
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = P(profile);
    sb.countSuggestedBindings = (uint32_t)b.size();
    sb.suggestedBindings = b.data();
    XrResult r = xrSuggestInteractionProfileBindings(g_instance, &sb);
    if (XR_FAILED(r)) Log("XR: bindings for %s rejected: %s", profile, ResultStr(r));
}

bool CreateActions()
{
    g_handPath[0] = P("/user/hand/left");
    g_handPath[1] = P("/user/hand/right");
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(si.actionSetName, "teardownvr");
    strcpy(si.localizedActionSetName, "Teardown VR");
    if (XR_FAILED(XR_LOG_FAIL(xrCreateActionSet(g_instance, &si, &g_actionSet)))) return false;

    // (the names a runtime's binding screen shows: in the language in use when the session is made - the runtime
    // keeps them; each must differ from the others)
    aAim = MakeAction("aim", Tr("Aim", "Visee"), XR_ACTION_TYPE_POSE_INPUT);
    aGrip = MakeAction("grip", Tr("Hand", "Main"), XR_ACTION_TYPE_POSE_INPUT);
    aStick = MakeAction("stick", Tr("Thumbstick", "Joystick"), XR_ACTION_TYPE_VECTOR2F_INPUT);
    aTrigger = MakeAction("trigger", Tr("Trigger", "Gachette"), XR_ACTION_TYPE_FLOAT_INPUT);
    aSqueeze = MakeAction("squeeze", Tr("Grip", "Poignee"), XR_ACTION_TYPE_FLOAT_INPUT);
    aStickClick = MakeAction("stick_click", Tr("Thumbstick click", "Clic joystick"), XR_ACTION_TYPE_BOOLEAN_INPUT);
    aPrimary = MakeAction("primary", Tr("A/X button", "Bouton A/X"), XR_ACTION_TYPE_BOOLEAN_INPUT);
    aSecondary = MakeAction("secondary", Tr("B/Y button", "Bouton B/Y"), XR_ACTION_TYPE_BOOLEAN_INPUT);
    aMenu = MakeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    aHaptic = MakeAction("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    SuggestBindings("/interaction_profiles/oculus/touch_controller", {
        { aAim, "/user/hand/left/input/aim/pose" }, { aAim, "/user/hand/right/input/aim/pose" },
        { aGrip, "/user/hand/left/input/grip/pose" }, { aGrip, "/user/hand/right/input/grip/pose" },
        { aStick, "/user/hand/left/input/thumbstick" }, { aStick, "/user/hand/right/input/thumbstick" },
        { aTrigger, "/user/hand/left/input/trigger/value" }, { aTrigger, "/user/hand/right/input/trigger/value" },
        { aSqueeze, "/user/hand/left/input/squeeze/value" }, { aSqueeze, "/user/hand/right/input/squeeze/value" },
        { aStickClick, "/user/hand/left/input/thumbstick/click" }, { aStickClick, "/user/hand/right/input/thumbstick/click" },
        { aPrimary, "/user/hand/left/input/x/click" }, { aPrimary, "/user/hand/right/input/a/click" },
        { aSecondary, "/user/hand/left/input/y/click" }, { aSecondary, "/user/hand/right/input/b/click" },
        { aMenu, "/user/hand/left/input/menu/click" },
        { aHaptic, "/user/hand/left/output/haptic" }, { aHaptic, "/user/hand/right/output/haptic" },
    });
    SuggestBindings("/interaction_profiles/khr/simple_controller", {
        { aAim, "/user/hand/left/input/aim/pose" }, { aAim, "/user/hand/right/input/aim/pose" },
        { aGrip, "/user/hand/left/input/grip/pose" }, { aGrip, "/user/hand/right/input/grip/pose" },
        { aTrigger, "/user/hand/left/input/select/click" }, { aTrigger, "/user/hand/right/input/select/click" },
        { aMenu, "/user/hand/left/input/menu/click" },
        { aHaptic, "/user/hand/left/output/haptic" }, { aHaptic, "/user/hand/right/output/haptic" },
    });

    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &g_actionSet;
    if (XR_FAILED(XR_LOG_FAIL(xrAttachSessionActionSets(g_session, &ai)))) return false;

    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.poseInActionSpace.orientation.w = 1;
        sci.subactionPath = g_handPath[i];
        sci.action = aAim;
        xrCreateActionSpace(g_session, &sci, &g_aimSpace[i]);
        sci.action = aGrip;
        xrCreateActionSpace(g_session, &sci, &g_gripSpace[i]);
    }
    return true;
}

bool CreateSession()
{
    XrGraphicsRequirementsD3D12KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    if (!pGetD3D12Req || XR_FAILED(XR_LOG_FAIL(pGetD3D12Req(g_instance, g_system, &req)))) return false;
    LUID luid = g_device->GetAdapterLuid();
    if (memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0)
        Log("XR: WARNING the headset runtime wants another GPU than the game uses");

    XrGraphicsBindingD3D12KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = g_device;
    binding.queue = g_queue;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = g_system;
    XrResult r = xrCreateSession(g_instance, &sci, &g_session);
    if (XR_FAILED(r)) {
        Log("XR: xrCreateSession failed: %s", ResultStr(r));
        g_status.state = { "Could not create the VR session", "Impossible de créer la session VR" };
        g_session = XR_NULL_HANDLE;
        return false;
    }

    // Reference spaces: STAGE (floor level) if available, LOCAL otherwise.
    uint32_t n = 0;
    xrEnumerateReferenceSpaces(g_session, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> types(n);
    xrEnumerateReferenceSpaces(g_session, n, &n, types.data());
    g_stage = std::find(types.begin(), types.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != types.end();
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = g_stage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace(g_session, &rs, &g_appSpace))) {
        g_stage = false;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        xrCreateReferenceSpace(g_session, &rs, &g_appSpace);
    }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(g_session, &rs, &g_viewSpace);
    Log("XR: session created (%s space)", g_stage ? "STAGE" : "LOCAL");

    // Swapchain colour format.
    xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(g_session, n, &n, fmts.data());
    const DXGI_FORMAT prefs[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
    g_colorFormat = fmts.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : (DXGI_FORMAT)fmts[0];
    for (DXGI_FORMAT p : prefs)
        if (std::find(fmts.begin(), fmts.end(), (int64_t)p) != fmts.end()) { g_colorFormat = p; break; }
    Log("XR: swapchain format %d", g_colorFormat);

    if (!CreateActions()) Log("XR: controller actions unavailable");

    if (pEnumRates) {
        uint32_t c = 0;
        pEnumRates(g_session, 0, &c, nullptr);
        g_status.refreshRates.resize(c);
        pEnumRates(g_session, c, &c, g_status.refreshRates.data());
        std::string list;
        for (float r : g_status.refreshRates) list += std::to_string((int)(r + 0.5f)) + " ";
        Log("XR: headset refresh rates: %s", list.c_str());
    }
    g_recenterPending = true;
    g_uiPlacePending = true;
    g_status.state = { "Session created, waiting for the headset...", "Session créée, en attente du casque..." };
    return true;
}

bool CreateSwapchain(Swapchain& sc, int w, int h)
{
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g_colorFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(XR_LOG_FAIL(xrCreateSwapchain(g_session, &ci, &sc.handle)))) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& i : imgs) sc.images.push_back(i.texture);
    sc.w = w;
    sc.h = h;
    Log("XR: swapchain %dx%d (%u images)", w, h, n);
    return true;
}

// Diagnostics: a headset runtime call that blocks the game (stutter) is logged.
struct XrCallTimer
{
    const char* name;
    double t0;
    explicit XrCallTimer(const char* n) : name(n), t0(ProfNow()) {}
    ~XrCallTimer()
    {
        double ms = ProfNow() - t0;
        static int logged = 0;
        if (ms > 15.0 && (logged < 60 || logged % 50 == 0)) Log("XR: %s blocked %.1f ms", name, ms);
        if (ms > 15.0) ++logged;
    }
};

ID3D12Resource* Acquire(Swapchain& sc)
{
    if (!sc.handle) return nullptr;
    if (sc.acquired) return nullptr;
    uint32_t idx = 0;
    XrResult ar;
    {
        XrCallTimer t("xrAcquireSwapchainImage");
        ar = xrAcquireSwapchainImage(sc.handle, nullptr, &idx);
    }
    if (XR_FAILED(ar)) return nullptr;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = 100000000; // 100 ms
    XrResult wr;
    {
        XrCallTimer t("xrWaitSwapchainImage");
        wr = xrWaitSwapchainImage(sc.handle, &wi);
    }
    if (XR_FAILED(wr)) {
        xrReleaseSwapchainImage(sc.handle, nullptr);
        return nullptr;
    }
    sc.acquired = true;
    return sc.images[idx];
}

void Release(Swapchain& sc)
{
    if (!sc.acquired) return;
    XrCallTimer t("xrReleaseSwapchainImage");
    xrReleaseSwapchainImage(sc.handle, nullptr);
    sc.acquired = false;
}

void EnsureSwapchains()
{
    Settings& s = Cfg();
    // (fovCrop: an eye image covers the headset's whole field, the back buffer only the part the game draws)
    int ew = std::max(64, (int)(g_bbW * s.eyeResScale / g_cropBase));
    int eh = std::max(64, (int)(g_bbH * s.eyeResScale / g_cropBase));
    if (g_eyeSc[0].w != ew || g_eyeSc[0].h != eh) {
        // Never resize in the middle of a stereo pair.
        if (g_leftDone) return;
        Render::WaitIdle();
        for (auto& e : g_eyeSc) { DestroySwapchain(e); CreateSwapchain(e, ew, eh); }
    }
    g_status.eyeImageW = g_eyeSc[0].w;   // (settings panel: the size the "Résolution envoyée" slider gives)
    g_status.eyeImageH = g_eyeSc[0].h;
    int sw = g_bbW, sh = g_bbH;
    if (sw > 2560) { sh = sh * 2560 / sw; sw = 2560; }
    if (g_screenSc.w != sw || g_screenSc.h != sh) {
        Render::WaitIdle();
        DestroySwapchain(g_screenSc);
        CreateSwapchain(g_screenSc, sw, sh);
    }
    if (!g_uiSc.handle) CreateSwapchain(g_uiSc, Ui::kWidth, Ui::kHeight);
}

void ApplyRefreshRate()
{
    int want = Cfg().refreshRate;
    if (!pRequestRate || want == g_appliedRefresh) return;
    g_appliedRefresh = want;
    if (want <= 0) return;
    float best = 0;
    for (float r : g_status.refreshRates)
        if (std::abs(r - want) < std::abs(best - want)) best = r;
    if (best > 0) {
        XrResult r = pRequestRate(g_session, best);
        Log("XR: request refresh %.1f Hz -> %s", best, ResultStr(r));
    }
}

void HandleEvents()
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (g_instance && xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            g_state = e->state;
            Log("XR: session state -> %d", g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(XR_LOG_FAIL(xrBeginSession(g_session, &bi)))) {
                    g_running = true;
                    g_status.state = { "VR active", "VR active" };
                }
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (auto& sc : g_eyeSc) Release(sc);
                Release(g_screenSc);
                Release(g_uiSc);
                xrEndSession(g_session);
                g_running = false;
                g_frameBegun = false;
                g_leftDone = false;
                g_status.state = { "Session paused (headset taken off?)", "Session en pause (casque retiré ?)" };
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                std::lock_guard<std::mutex> lock(g_mutex);
                DestroySession();
                g_status.state = { "Session ended, retrying...", "Session terminée, nouvelle tentative..." };
                g_nextRetry = Now() + 3.0;
            }
            g_status.focused = g_state == XR_SESSION_STATE_FOCUSED;
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
            std::lock_guard<std::mutex> lock(g_mutex);
            DestroyInstance();
            g_nextRetry = Now() + 5.0;
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            g_recenterPending = true;
            break;
        case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
            // (the hidden-area fill asks for it again at the next eye image)
            if (g_hidden.fetched) Log("XR: the runtime changed its lens mask");
            g_hidden.fetched = false;
            break;
        default:
            break;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// ---------------------------------------------------------------------------
// Headset runtime ([Affichage] headsetRuntime, config.h)
// ---------------------------------------------------------------------------
// 0 = automatic (default): when the game starts, the runtimes that may have the Quest are asked in turn whether
// a headset is there, and the game's instance is made on the first that has it. Each is asked with an instance
// of its own (xrCreateInstance + xrGetSystem + xrDestroyInstance, as the start-up check always did for Windows'
// runtime) after XR_RUNTIME_JSON was set to its .json: the bundled loader reads that variable whenever an
// xrCreateInstance has to load a runtime, and xrDestroyInstance unloads it (third_party/openxr: loader_core.cpp
// LoaderXrDestroyInstance -> RuntimeInterface::UnloadRuntime; runtime_interface.cpp LoadRuntime ->
// manifest_file.cpp FindManifestFiles -> GetEnvironmentVariableW). One instance at a time: the loader refuses a
// second one (XR_ERROR_LIMIT_REACHED), so the game's own is destroyed before the runtimes are asked again.
// Order: Windows' active runtime (both connected: Windows' choice wins), Virtual Desktop, Meta Quest Link;
// SteamVR only when it is Windows' runtime, and last (asking it starts SteamVR). Meta's runtime is only asked
// while the headset signal is set (HmdSignal): its own xrGetSystem checks that signal first and answers
// XR_ERROR_FORM_FACTOR_UNAVAILABLE without it (LibOVRRTImpl64_1.dll +0xebc7f), so nothing is lost and Meta's
// runtime is not even loaded while no headset is on Quest Link. Virtual Desktop's runtime only reads its
// streamer's shared memory (it imports no CreateProcess / ShellExecute: nothing is started).
struct RuntimeChoice
{
    bool autoMode = false;               // headsetRuntime 0 and no [Debug] openxrRuntime
    bool test = false;                   // [Debug] xrAutoRuntimes: those runtimes instead of the real ones
    std::vector<std::string> testList;
    std::string chosen;                  // the .json of the game's instance ("" = the loader's default)
    int signalChecks = 0;                // retries in a row with the signal set and no headset on the instance
    double rearmAt = 0;                  // no new detection before this Now()
    VrText text;                         // what the settings panel shows (Infos tab), in both languages, and its version
    unsigned textSerial = 0;
};
RuntimeChoice g_rt;
unsigned g_rtTextShown = 0;
const double kDetectBudget = 3.0;        // no runtime is asked any more once the detection took this long (s)

void SetRuntimeText(const std::string& en, const std::string& fr)
{
    g_rt.text = { en, fr };
    ++g_rt.textSerial;
}

// A runtime's name (log, settings panel).
std::string RuntimeLabel(const std::string& json)
{
    switch (ConfigXrRuntimeKind(json)) {
    case 1: return "Virtual Desktop";
    case 2: return "Meta Quest Link";
    case 3: return "SteamVR";
    }
    if (json.empty()) return "none";
    const size_t slash = json.find_last_of("\\/");
    return slash == std::string::npos ? json : json.substr(slash + 1);
}

// The headset signal, the event "OculusHMDConnected": Meta's server sets it while a headset is on Quest Link /
// Air Link (OVRServer_x64.exe +0x24f55c: CreateEventW, manual reset - NtQueryEvent: notification type); Virtual
// Desktop's streamer uses the same name for its own headset. -1 = no such event (neither of them runs), 0 clear,
// 1 set. Read with a 0 ms wait, exactly as Meta's runtime reads it: a manual-reset event is not changed by it.
int HmdSignal()
{
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, L"OculusHMDConnected");
    if (!h) return -1;
    const int set = WaitForSingleObject(h, 0) == WAIT_OBJECT_0 ? 1 : 0;
    CloseHandle(h);
    return set;
}

const char* SignalText(int s)
{
    return s < 0 ? "absent (neither Meta's server nor Virtual Desktop runs)" : s ? "set" : "clear";
}

// Asks one runtime whether a headset is there: an instance of its own (the loader loads the runtime), its system,
// the instance destroyed (the loader unloads the runtime). json nullptr: the runtime XR_RUNTIME_JSON names now
// (ConfigLoad: chosen in the settings, [Debug] openxrRuntime) or Windows' one - the start-up check as before.
bool AskRuntime(const char* json, const char* name)
{
    if (json) SetEnvironmentVariableA("XR_RUNTIME_JSON", json);
    const double t0 = Now();
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "Teardown VR probe");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance inst = XR_NULL_HANDLE;
    const XrResult rc = xrCreateInstance(&ci, &inst);
    XrResult rs = XR_ERROR_RUNTIME_UNAVAILABLE;
    if (XR_SUCCEEDED(rc)) {
        XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
        gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        rs = xrGetSystem(inst, &gi, &sys);
        xrDestroyInstance(inst);
    }
    const bool there = XR_SUCCEEDED(rc) && XR_SUCCEEDED(rs);
    char why[64];
    if (there) snprintf(why, sizeof(why), "headset there");
    else if (XR_FAILED(rc)) snprintf(why, sizeof(why), "not started (xrCreateInstance %d)", (int)rc);
    else snprintf(why, sizeof(why), "no headset (xrGetSystem %d)", (int)rs);
    Log("XR: asked %s (%s): %s, %.0f ms", name, json ? json : "XR_RUNTIME_JSON as set, or Windows' runtime", why,
        (Now() - t0) * 1000.0);
    return there;
}

// headsetRuntime 0: chooses the runtime of the game's instance - the first in the order above that has the headset
// - and names it in XR_RUNTIME_JSON. Returns whether one had it. None: the game waits on Windows' runtime
// (RuntimeRedetect asks again when the headset signal comes). later: asked again after the start.
bool DetectRuntime(bool later)
{
    const double t0 = Now();
    std::vector<std::string> asked;
    auto ask = [&](const std::string& json, const std::string& name) {
        if (json.empty() || std::find(asked.begin(), asked.end(), json) != asked.end()) return false;
        if (Now() - t0 > kDetectBudget) {
            Log("XR: %s not asked: the detection already took %.1f s", name.c_str(), Now() - t0);
            return false;
        }
        asked.push_back(json);
        return AskRuntime(json.c_str(), name.c_str());
    };
    // (read only: the registry, and the signal)
    const std::string active = ConfigActiveXrRuntime(), vd = ConfigFindXrRuntime(1), meta = ConfigFindXrRuntime(2);
    const int kind = ConfigXrRuntimeKind(active), signal = HmdSignal();
    Log("XR: headset detection (%s)%s - Windows' runtime: %s (%s); Virtual Desktop: %s; Meta Quest Link: %s; "
        "headset signal OculusHMDConnected: %s", later ? "headset signal" : "game start",
        g_rt.test ? " TEST [Debug] xrAutoRuntimes, the real runtimes are only read" : "",
        active.empty() ? "none" : active.c_str(), RuntimeLabel(active).c_str(), vd.empty() ? "not installed" : vd.c_str(),
        meta.empty() ? "not installed" : meta.c_str(), SignalText(signal));
    std::string pick, fallback;
    if (g_rt.test) {
        for (const std::string& j : g_rt.testList)
            if (ask(j, "test " + RuntimeLabel(j))) { pick = j; break; }
        fallback = g_rt.testList.front();
    } else {
        // 1. Windows' runtime (both connected: its choice wins) - SteamVR not yet, Meta's only with the signal
        if (kind != 3 && (kind != 2 || signal == 1) && ask(active, "Windows' runtime " + RuntimeLabel(active)))
            pick = active;
        // 2. Virtual Desktop
        if (pick.empty() && ask(vd, "Virtual Desktop")) pick = vd;
        // 3. Meta Quest Link, while the signal is set
        if (pick.empty() && !meta.empty() && std::find(asked.begin(), asked.end(), meta) == asked.end()) {
            if (signal == 1) {
                if (ask(meta, "Meta Quest Link")) pick = meta;
            } else {
                Log("XR: Meta Quest Link not asked: no headset on Quest Link (signal %s - Meta's runtime checks it "
                    "first and would answer no)", SignalText(signal));
            }
        }
        // 4. SteamVR when it is Windows' runtime: the game would start it anyway
        if (pick.empty() && kind == 3 && ask(active, "Windows' runtime SteamVR")) pick = active;
        fallback = !active.empty() ? active : !vd.empty() ? vd : meta;
    }
    const bool found = !pick.empty();
    g_rt.chosen = found ? pick : fallback;
    SetEnvironmentVariableA("XR_RUNTIME_JSON", g_rt.chosen.empty() ? nullptr : g_rt.chosen.c_str());
    const std::string label = RuntimeLabel(g_rt.chosen);
    const double ms = (Now() - t0) * 1000.0;
    if (found) {
        Log("XR: headset detection: %s has the headset - the game uses %s (%d runtime(s) asked, %.0f ms)",
            label.c_str(), g_rt.chosen.c_str(), (int)asked.size(), ms);
        SetRuntimeText(label + (later ? " (headset connected after launch)" : " (headset found at launch)"),
                       label + (later ? " (casque connecté après le lancement)" : " (casque trouvé au lancement)"));
    } else if (g_rt.chosen.empty()) {
        Log("XR: headset detection: no OpenXR runtime installed (%.0f ms)", ms);
        SetRuntimeText("no OpenXR runtime installed", "aucun runtime OpenXR installé");
    } else {
        Log("XR: headset detection: no headset (%d runtime(s) asked, %.0f ms) - the game waits on %s (%s); a headset "
            "connected later to Quest Link or Virtual Desktop is found then", (int)asked.size(), ms, label.c_str(),
            g_rt.chosen.c_str());
        SetRuntimeText("no headset at launch, waiting on " + label, "aucun casque au lancement, en attente sur " + label);
    }
    return found;
}

// The runtime of the game's instance, chosen once (HeadsetInUse, before the game makes its window; PumpXr when
// that did not run). Returns whether a headset was there then.
bool RuntimeStartup()
{
    static std::once_flag once;
    static bool found = false;
    std::call_once(once, [] {
        g_rt.autoMode = ConfigXrAutoDetect();
        if (!g_rt.autoMode) {
            // (a runtime set in the settings or by [Debug] openxrRuntime - XR_RUNTIME_JSON, ConfigLoad - or
            // Windows' one with headsetRuntime 4: only asked, as before)
            found = AskRuntime(nullptr, "the runtime set in the settings");
            if (Cfg().headsetRuntime < 1 || Cfg().headsetRuntime > 4)
                SetRuntimeText("forced by [Debug] openxrRuntime", "imposé par [Debug] openxrRuntime");
            return;
        }
        const std::string& list = ConfigXrAutoTestRuntimes();
        for (size_t a = 0; a <= list.size();) {
            size_t b = list.find(';', a);
            if (b == std::string::npos) b = list.size();
            std::string j = list.substr(a, b - a);
            j.erase(0, j.find_first_not_of(" \t\""));
            j.erase(j.find_last_not_of(" \t\"") + 1);
            if (!j.empty()) g_rt.testList.push_back(j);
            a = b + 1;
        }
        g_rt.test = !g_rt.testList.empty();
        found = DetectRuntime(false);
    });
    return found;
}

// headsetRuntime 0, the game's instance has no headset (xrGetSystem, every 3 s) - or the runtime it waits on does
// not even start (CreateInstance fails: Virtual Desktop still Windows' runtime with its streamer closed): the Quest
// may have been connected since the start to another runtime (Quest Link while Virtual Desktop stayed Windows'
// runtime). Once the headset signal was set at two checks in a row (the instance's own runtime, if it is the one,
// says yes by then), the instance (if any) is destroyed and the runtimes are asked again - at most every 15 s.
// Returns true when they were asked (the next retry makes the chosen runtime's instance). Test list: the signal
// counts as always set, every 9 s.
bool RuntimeRedetect()
{
    if (!g_rt.autoMode || Now() < g_rt.rearmAt) return false;
    const int signal = g_rt.test ? 1 : HmdSignal();
    g_rt.signalChecks = signal == 1 ? g_rt.signalChecks + 1 : 0;
    if (g_rt.signalChecks < 2) return false;
    g_rt.signalChecks = 0;
    g_rt.rearmAt = Now() + (g_rt.test ? 9.0 : 15.0);
    const std::string before = g_rt.chosen;
    Log("XR: headset signal set%s while %s has no headset - the runtimes are asked again",
        g_rt.test ? " (test list: always)" : "", RuntimeLabel(before).c_str());
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        DestroyInstance();
    }
    if (DetectRuntime(true) && g_rt.chosen != before)
        Log("XR: switched to %s (%s) - the headset was connected after the game started", RuntimeLabel(g_rt.chosen).c_str(),
            g_rt.chosen.c_str());
    return true;
}

void PumpXr()
{
    // (the runtime is chosen once - normally already by HeadsetInUse, before the game made its window)
    RuntimeStartup();
    if (g_rt.textSerial != g_rtTextShown) {
        g_rtTextShown = g_rt.textSerial;
        g_status.runtimeDetect = g_rt.text;
    }
    if (!g_session && Now() >= g_nextRetry) {
        g_nextRetry = Now() + 3.0;
        // (automatic choice, RuntimeRedetect: connected to another runtime since the start? - asked when the
        // waiting runtime does not start, or has no headset)
        if (!g_instance && !CreateInstance()) {
            if (RuntimeRedetect()) g_nextRetry = 0;
            return;
        }
        if (g_system == XR_NULL_SYSTEM_ID && !GetSystem()) {
            if (g_instance && RuntimeRedetect()) g_nextRetry = 0;
            return;
        }
        CreateSession();
    }
    HandleEvents();
    // ([Performance] cpuPriority: "above normal" while the session runs, normal again after)
    Perf::SessionPriority(g_running);
    // ([Performance] gpuPriority: the GPU class "below normal" while it runs; vdStats: Virtual Desktop's
    // encoder settings logged, its holds counted, the PC's network send rate sampled)
    Perf::GpuPriority(g_running);
    Perf::StreamSession(g_running, g_status.runtime);
    g_status.sessionRunning = g_running;
    if (g_running) ApplyRefreshRate();
    if (pGetRate && g_running) pGetRate(g_session, &g_status.refreshRate);
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------
// Whether the controllers' state read by LocateHands is real input (g_mutex held). An inactive
// action reads 0 - a grip "released" while the player still squeezes it: our floating panels
// (UpdatePanelGrips) then wait instead of taking it for a release.
bool g_actionsLive = false;     // the last xrSyncActions returned XR_SUCCESS (XR_SESSION_NOT_FOCUSED
                                // is a success code too: the session lost the focus, every action inactive)
bool g_handInput[2] = {};       // that hand's grip and trigger were active (bound, controller on)

void LocateHands(XrTime t)
{
    g_actionsLive = false;
    if (!g_actionSet) return;
    XrActiveActionSet aas = { g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    g_actionsLive = xrSyncActions(g_session, &si) == XR_SUCCESS;

    for (int i = 0; i < 2; ++i) {
        HandState& h = g_handsRaw[i];
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.subactionPath = g_handPath[i];

        auto getF = [&](XrAction a, bool* active = nullptr) {
            XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
            gi.action = a;
            xrGetActionStateFloat(g_session, &gi, &s);
            if (active) *active = s.isActive != XR_FALSE;
            return s.isActive ? s.currentState : 0.0f;
        };
        auto getB = [&](XrAction a) {
            XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
            gi.action = a;
            xrGetActionStateBoolean(g_session, &gi, &s);
            return s.isActive && s.currentState;
        };
        XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
        gi.action = aStick;
        xrGetActionStateVector2f(g_session, &gi, &v);
        h.stickX = v.isActive ? v.currentState.x : 0;
        h.stickY = v.isActive ? v.currentState.y : 0;
        bool trigOn = false, gripOn = false;
        h.trigger = getF(aTrigger, &trigOn);
        h.squeeze = getF(aSqueeze, &gripOn);
        g_handInput[i] = trigOn && gripOn;
        h.stickClick = getB(aStickClick);
        h.primary = getB(aPrimary);
        h.secondary = getB(aSecondary);
        h.menu = getB(aMenu);

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        bool ok = XR_SUCCEEDED(xrLocateSpace(g_aimSpace[i], g_appSpace, t, &loc)) &&
                  (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (ok) h.aim = ToPose(loc.pose);
        XrSpaceLocation gl = { XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrLocateSpace(g_gripSpace[i], g_appSpace, t, &gl)) &&
            (gl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            h.grip = ToPose(gl.pose);
        h.active = ok;
    }
}

void ApplyRecenter()
{
    float yaw = YawOf(g_headRaw.q);
    g_recenter.q = AxisAngle({ 0, 1, 0 }, yaw);
    g_recenter.p = { g_headRaw.p.x, 0, g_headRaw.p.z };
    g_refHeadY = g_headRaw.p.y;
    g_recenterPending = false;
    g_recenterCount++;
    g_uiPlacePending = true;
    g_panelSnap = true;
    Log("XR: recentered (yaw %.1f deg, head height %.2f m)", yaw / kDeg, g_refHeadY);
}

// ---------------------------------------------------------------------------
// Floating panels: the VR inventory, the settings / keys panel
// ---------------------------------------------------------------------------
// Both are quad layers in room (app) space, composited by the runtime with the newest head pose
// (no game-frame lag, identical in both eyes of the synchronised stereo pair, no jump between
// the A and B frames). Like windows floating in the room, they do not move when the head turns:
// the player turns the head to look at them (before round 16 the inventory followed the head
// within half a second - the player's complaint).
//  - The settings / keys panel opens straight in front of the gaze (uiPanelDistance ahead of
//    the eyes, uiPanelDown below them: PlaceUiPanel) and stays there in the room. Stick walking
//    and turning move the world, not the room: it stays in front of the player.
//  - The VR inventory is worn by the body (g_body, UpdateBodyAnchor): in front of it at the
//    belt, or where the player put it (inventoryYaw around the body, inventoryForward ahead of
//    the eyes, inventoryDown below them). The body is estimated in room space from the head,
//    not from the game's player transform (locomotion moves the world: in room space the body
//    is where the head is).
//  - Either one is moved with a grip (UpdatePanelGrips, UpdatePanels): pressed while that hand's
//    ray is on the panel, the press belongs to the panel until released - never the game's
//    grab. The point under the ray stays under it ("ray drag": the hand's translation moves it
//    1:1, a turn of the wrist sweeps it); the panel stays upright and faces the eyes. Let go, the
//    place is kept (only those keys are written to the ini, off this thread: ConfigSaveKeys).
// One kind of place for both: a direction around an anchor, a horizontal distance ahead of the
// eye point and a height below it; the eye point is the anchor + eyeFromAnchor turned toward the
// panel (the neck turns the head toward it).
struct FloatPanel
{
    Vec3 eyeFromAnchor;       // the eye point from the anchor, in the frame turned toward the panel
    float fwdMin, fwdMax;     // horizontal distance ahead of the eye point, metres
    float downMin, downMax;   // height below the eye point, metres (negative = above)
    float tiltBias;           // tilt = atan2(down - tiltBias, fwd): 0 = faces the eye point
};
const Vec3 kEyeFromNeck = { 0, 0.11f, -0.08f };   // eyes from the neck pivot (11 cm above, 8 cm ahead)
// Limits. Quad layers are drawn over the game's image without a depth test: a panel far away
// would still show in front of a nearer wall (uncomfortable in stereo), so the inventory stays
// within 1 m of the eyes, the settings panel within 1.5 m.
const FloatPanel kInvPanel = { kEyeFromNeck, 0.20f, 1.00f, -0.40f, 0.90f, 0.0f };
// (anchored at the eyes; vertical at its default place, 0.15 m below them, as before round 16)
const FloatPanel kUiPanel = { { 0, 0, 0 }, 0.40f, 1.50f, -0.50f, 0.80f, 0.15f };
constexpr float kFollowPanelDeg = 50;   // body turn: also this far from the inventory's direction
constexpr float kFocusDeg = 35;         // the inventory's trigger works within this angle of the gaze
constexpr float kRecallDeg = 65;        // R3 brings it back from beyond this angle (VrInventoryRecall)

struct Anchor { Vec3 pos; float yaw = 0; };
// A panel's place around its anchor. yaw: radians about +Y from the anchor's direction (+ = to the
// LEFT, as AxisAngle; the ini's inventoryYaw is in degrees, + = to the right).
struct PanelPlace { float yaw = 0, fwd = 0, down = 0; };

float WrapAngle(float a)
{
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

// Heading of the head, also defined when it looks straight down or up (the
// direction of the top of the head then gives it).
float HeadHeading(Quat q)
{
    Vec3 f = Rotate(q, { 0, 0, -1 }), u = Rotate(q, { 0, 1, 0 });
    float k = f.y < 0 ? 1.0f : -1.0f;
    return std::atan2(-(f.x + k * u.x), -(f.z + k * u.z));
}

// The panel's pose (app space) for a place. Upright (no roll), turned toward the eye point and
// tilted to face it: for the inventory exactly the formula used since round 11.
Pose PanelPose(const FloatPanel& fp, const Anchor& a, const PanelPlace& o)
{
    const Quat yq = AxisAngle({ 0, 1, 0 }, a.yaw + o.yaw);
    Pose p;
    p.p = a.pos + Rotate(yq, fp.eyeFromAnchor) + Rotate(yq, { 0, -o.down, -o.fwd });
    p.q = yq * AxisAngle({ 1, 0, 0 }, -std::atan2(o.down - fp.tiltBias, o.fwd));
    return p;
}

// The inverse: the place of a panel centred at c, within the panel's limits (exact round trip
// inside them). A centre straight above or below the anchor keeps the previous direction.
PanelPlace PlaceFromCentre(const FloatPanel& fp, const Anchor& a, Vec3 c, float prevYaw)
{
    const Vec3 r = c - a.pos;
    const float hd = std::sqrt(r.x * r.x + r.z * r.z);
    PanelPlace o;
    o.yaw = hd > 0.05f ? WrapAngle(std::atan2(-r.x, -r.z) - a.yaw) : prevYaw;
    o.fwd = std::clamp(hd + fp.eyeFromAnchor.z, fp.fwdMin, fp.fwdMax);   // (eyeFromAnchor.z < 0: the eyes are ahead)
    o.down = std::clamp(fp.eyeFromAnchor.y - r.y, fp.downMin, fp.downMax);
    return o;
}

// The VR inventory's saved place (settings, within its limits).
PanelPlace InvPlace()
{
    const Settings& s = Cfg();
    PanelPlace o;
    o.yaw = WrapAngle(-s.inventoryYaw * kDeg);
    o.fwd = std::clamp(s.inventoryForward, kInvPanel.fwdMin, kInvPanel.fwdMax);
    o.down = std::clamp(s.inventoryDown, kInvPanel.downMin, kInvPanel.downMax);
    return o;
}

// --- Which hand moves which panel (present thread; g_gripOwned, g_trigMasked and g_holdHand are
// also read by the Lua thread - VrHandleLuaRequest - and by input.cpp) ---
// (kPanelModWindow: a mod's menu shown as a floating panel by the engine - engine.cpp, ModPanel)
enum PanelId { kPanelNone, kPanelSettings, kPanelInventory, kPanelModWindow };
struct PanelHold
{
    int hand = -1, panel = kPanelNone;   // the hand moving a panel, and which panel
    float dist = 0;                      // the grab point: metres along that hand's ray,
    float lx = 0, ly = 0;                // ... and from the panel's centre (metres, x right, y up)
    bool gripDown[2] = {};               // each grip (hysteresis 0.6 / 0.4, like the game's grab in input.cpp)
    unsigned long long modSession = 0;   // a mod's window: its session (EngineModPanel::session)
} g_hold;
std::atomic<bool> g_gripOwned[2];        // that grip's press belongs to a panel, until it is released
std::atomic<bool> g_trigMasked[2];       // that hand moves a panel, or did and its trigger is still held
std::atomic<int> g_holdHand{ -1 };
bool g_gripQuiet[2] = {};                // Lua sees no grip on that hand: the panel's press, until it is
                                         // nearly open (< 0.25) or squeezed again (VrHandleLuaRequest)
float g_quietMin[2] = {};                // ... the grip's lowest value since the panel's press ended
double g_gripSeenAt[2] = {};             // last UpdatePanelGrips with real input from that hand (Now())
int g_presentsUnseen[2] = {};            // presents since then (UpdatePanels: a long game frame alone
                                         // is not missing input)

const char* PanelName(int panel)
{
    return panel == kPanelInventory   ? "inventory"
           : panel == kPanelModWindow ? "mod window"
           : Ui::KeysShown()          ? "keys panel"
                                      : "settings panel";
}

// The settings / keys panel straight in front of the gaze (BeginXrFrame, g_mutex held): at every
// opening, from its button, after a recenter. Never while a hand moves it.
void PlaceUiPanel()
{
    g_uiPlacePending = false;
    if (g_hold.panel == kPanelSettings) return;
    const Settings& s = Cfg();
    PanelPlace o;
    o.yaw = HeadHeading(g_headRaw.q);
    o.fwd = std::clamp(s.uiPanelDistance, kUiPanel.fwdMin, kUiPanel.fwdMax);
    o.down = std::clamp(s.uiPanelDown, kUiPanel.downMin, kUiPanel.downMax);
    g_uiPose = PanelPose(kUiPanel, { g_headRaw.p, 0 }, o);
}

// The VR inventory's anchor: the body, estimated in room space from the head.
//  - Position: a neck pivot 11 cm below and 8 cm behind the eyes (looking down at the panel turns
//    the head about it: the panel does not move away). It follows the neck with hysteresis: a step
//    in the room (beyond 20 cm), crouching or standing up (8 cm) carries the panel, and the anchor
//    then goes all the way back onto the neck (within 2 cm, 1 cm); leaning in to read does not
//    move it.
//  - Yaw: fixed while the head turns. With inventoryFollow, a real body turn - the head more than
//    inventoryFollowAngle (55 degrees) from the body's direction and kFollowPanelDeg from the
//    panel's, for inventoryFollowDelay (1.2 s) - turns it to the head's heading (within
//    5 degrees). Looking at a panel parked at the side never does. Not while a hand moves it.
//  - While a hand moves the panel (held): the position is the neck itself, no hysteresis lag
//    (UpdateInventoryPanel keeps the panel's centre): the limits the player meets while moving
//    it are those of the place saved at release - no jump when it is let go near a limit.
struct BodyAnchor
{
    Anchor a;
    bool valid = false, followH = false, followV = false, turning = false;
    float turnT = 0;
} g_body;
bool g_invFocus = false;         // the player looks toward the inventory (its trigger works)
float g_invGazeDeg = 180;        // angle between the gaze and the inventory's centre
float g_invFrontGazeDeg = 180;   // ... once put back in front of the body (VrInventoryRecall)

void UpdateBodyAnchor(const Pose& head, float dt, bool snap, bool held, float panelYaw)
{
    const Vec3 neck = head.p - Rotate(head.q, kEyeFromNeck);
    const float heading = HeadHeading(head.q);
    BodyAnchor& b = g_body;
    if (snap || !b.valid) {
        b = BodyAnchor{};
        b.a = { neck, heading };
        b.valid = true;
        return;
    }
    if (held) {
        b.a.pos = neck;
        b.followH = b.followV = b.turning = false;
        b.turnT = 0;
        return;
    }
    dt = std::clamp(dt, 0.0f, 0.1f);
    const float k = 1 - std::exp(-dt / 0.2f);
    const float dx = neck.x - b.a.pos.x, dz = neck.z - b.a.pos.z, dy = neck.y - b.a.pos.y;
    const float dh = std::sqrt(dx * dx + dz * dz);
    if (dh > 0.20f) b.followH = true;
    if (b.followH) {
        b.a.pos.x += dx * k;
        b.a.pos.z += dz * k;
        if (dh * (1 - k) < 0.02f) b.followH = false;
    }
    if (std::abs(dy) > 0.08f) b.followV = true;
    if (b.followV) {
        b.a.pos.y += dy * k;
        if (std::abs(dy) * (1 - k) < 0.01f) b.followV = false;
    }
    if (!Cfg().inventoryFollow) {
        b.turning = false;
        b.turnT = 0;
        return;
    }
    const float diff = WrapAngle(heading - b.a.yaw);
    if (!b.turning) {
        const float angle = std::clamp(Cfg().inventoryFollowAngle, 20.0f, 170.0f) * kDeg;
        const bool away = std::abs(diff) > angle && std::abs(WrapAngle(diff - panelYaw)) > kFollowPanelDeg * kDeg;
        b.turnT = away ? b.turnT + dt : 0;
        if (b.turnT > std::clamp(Cfg().inventoryFollowDelay, 0.2f, 10.0f)) {
            b.turning = true;
            static int logs = 0;
            if (logs < 50) {
                ++logs;
                Log("Panels: body turned %+.0f deg (+ = right) for %.1f s - the inventory follows", -diff / kDeg, b.turnT);
            }
        }
    }
    if (b.turning) {
        b.a.yaw = WrapAngle(b.a.yaw + diff * (1 - std::exp(-dt / 0.25f)));
        if (std::abs(WrapAngle(heading - b.a.yaw)) < 5 * kDeg) {
            b.turning = false;
            b.turnT = 0;
        }
    }
}

float GazeAngleDeg(const Pose& head, Vec3 c)
{
    const Vec3 to = c - head.p;
    const float d = Length(to);
    if (d < 1e-3f) return 0;
    return std::acos(std::clamp(Dot(Rotate(head.q, { 0, 0, -1 }), to) / d, -1.0f, 1.0f)) / kDeg;
}

// Ray drag: the grab point (g_hold.dist along the holding hand's ray; g_hold.lx / ly from the
// centre, in the panel's plane) stays under the ray, the centre follows within the panel's limits
// (at a limit the panel stops and the ray slides off the grab point). Two passes: the panel's tilt
// depends on where it is. Updates the place o, returns the pose.
Pose DragPanel(const FloatPanel& fp, const Anchor& a, Pose p, PanelPlace& o, const HandState& hand)
{
    const Vec3 g = hand.aim.p + Rotate(hand.aim.q, { 0, 0, -1 }) * g_hold.dist;
    for (int pass = 0; pass < 2; ++pass) {
        o = PlaceFromCentre(fp, a, g - Rotate(p.q, { g_hold.lx, g_hold.ly, 0 }), o.yaw);
        p = PanelPose(fp, a, o);
    }
    return p;
}

// The VR inventory this present (present thread, while it is shown): the body anchor + its saved
// place, or, while a hand moves it, where that hand's ray puts it (the saved place follows live;
// holder = nullptr while that hand's tracking is lost: the panel waits). snap: it just appeared,
// R3 recall or a recenter - the anchor goes onto the neck, facing the head's heading.
void UpdateInventoryPanel(float dt, bool snap, const Pose& head, const HandState* holder)
{
    const bool held = g_hold.panel == kPanelInventory;
    PanelPlace o = InvPlace();
    // (held: the anchor goes onto the neck - UpdateBodyAnchor - and the place is measured again from
    // there, so the panel's centre does not move, within its limits; also while that hand's
    // tracking is lost and the panel waits)
    const bool keep = held && g_body.valid;
    const Vec3 c0 = keep ? PanelPose(kInvPanel, g_body.a, o).p : Vec3{};
    UpdateBodyAnchor(head, dt, snap && !held, held, o.yaw);
    if (keep) o = PlaceFromCentre(kInvPanel, g_body.a, c0, o.yaw);
    Pose p = PanelPose(kInvPanel, g_body.a, o);
    if (held) {
        if (holder) p = DragPanel(kInvPanel, g_body.a, p, o, *holder);
        Settings& s = Cfg();   // (the settings panel - its sliders - is closed while the inventory shows)
        s.inventoryYaw = -o.yaw / kDeg;
        s.inventoryForward = o.fwd;
        s.inventoryDown = o.down;
    }
    // Looking toward it: the panel's centre within 35 degrees of the head's direction (at the
    // default place: the head bent down 20 degrees or more). Looking ahead, at the floor a few
    // metres away for example, a ray that crosses the panel leaves the trigger and grips to the
    // game. (Held: always.)
    g_invGazeDeg = GazeAngleDeg(head, p.p);
    {
        std::lock_guard<std::mutex> lock(g_mutex);   // (the Lua thread reads them: OffLaserStop, mods' windows, grips)
        g_invPose = p;
        g_invFocus = held || g_invGazeDeg < kFocusDeg;
    }
    const Anchor front{ head.p - Rotate(head.q, kEyeFromNeck), HeadHeading(head.q) };
    g_invFrontGazeDeg = GazeAngleDeg(head, PanelPose(kInvPanel, front, o).p);
    // (log: where it appears; where it goes when the body carries it - a head turn alone: nothing)
    static Vec3 logged;
    static int logs = 0;
    const Vec3 moved = p.p - logged;
    if (snap && !held && logs < 200) {
        ++logs;
        Log("Panels: inventory shown at %+.0f deg (+ = right), %.3f m ahead, %.3f m below the eyes: centre (%.3f %.3f %.3f), "
            "facing %+.1f deg, tilt %.1f deg, %.0f deg from the gaze", -o.yaw / kDeg, o.fwd, o.down, p.p.x, p.p.y, p.p.z,
            -(g_body.a.yaw + o.yaw) / kDeg, std::atan2(o.down, o.fwd) / kDeg, g_invGazeDeg);
        logged = p.p;
    } else if (held) {
        logged = p.p;
    } else if (Length(moved) > 0.05f && logs < 200) {
        ++logs;
        Log("Panels: inventory carried by the body to (%.3f %.3f %.3f) - anchor (%.3f %.3f %.3f), facing %+.1f deg",
            p.p.x, p.p.y, p.p.z, g_body.a.pos.x, g_body.a.pos.y, g_body.a.pos.z, -g_body.a.yaw / kDeg);
        logged = p.p;
    }
}

void UpdatePanelGrips();   // (below the ray tests; BeginXrFrame)

double g_waitMs = 0;   // time spent in xrWaitFrame since the last stats line
int g_waits = 0;
double g_lastWaitMs = 0;   // the most recent xrWaitFrame
double g_waitMax = 0;
int g_waitLong = 0;        // xrWaitFrame calls that blocked more than 14 ms (runtime pacing)
int g_displayMissed = 0;   // headset refreshes that got no new image from the game since the last stats line
int g_displayMissedRt = 0; // ... of them while the runtime held xrEndFrame longer than a refresh (Virtual Desktop)
double g_lastEndMs = 0;    // the most recent xrEndFrame (0 = none since the last xrWaitFrame)
int g_holdsSmoothed = 0;   // [Performance] holdSmooth: runtime holds after which the game resumed where it stopped
double g_holdNotPlayedMs = 0;   // ... the game time they would have jumped ahead (since the last stats line)

void BeginXrFrame()
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    double w0 = Now();
    XrResult wr = xrWaitFrame(g_session, &wi, &fs);
    g_lastWaitMs = (Now() - w0) * 1000.0;
    g_waitMs += g_lastWaitMs;
    g_waitMax = std::max(g_waitMax, g_lastWaitMs);
    if (g_lastWaitMs > 14.0) ++g_waitLong;
    g_waits++;
    if (XR_FAILED(XR_LOG_FAIL(wr))) return;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult br;
    {
        XrCallTimer t("xrBeginFrame");
        br = xrBeginFrame(g_session, &bi);
    }
    if (XR_FAILED(XR_LOG_FAIL(br))) return;

    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = g_appSpace;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    XrResult r = xrLocateViews(g_session, &li, &vs, 2, &n, views);
    XrSpaceLocation hl = { XR_TYPE_SPACE_LOCATION };
    xrLocateSpace(g_viewSpace, g_appSpace, fs.predictedDisplayTime, &hl);

    // Simulation time step for the coming stereo pair = display interval.
    static XrTime lastDisplay = 0;
    if (lastDisplay && fs.predictedDisplayTime > lastDisplay) {
        XrTime delta = fs.predictedDisplayTime - lastDisplay;
        // [Performance] holdSmooth. When the runtime itself held the last xrEndFrame longer than a refresh
        // (Virtual Desktop waiting for its stream: the Wi-Fi or the headset's decoder - H.264+ at high
        // bitrates), the headset kept showing the last image meanwhile. Advancing the game by the whole gap
        // would make everything that moves jump ahead when the stream resumes, and the engine would run two
        // physics steps in the next frame A, making it heavier right when the stream is fragile (holds then
        // come in chains). The game resumes where it stopped instead: one refresh of game time; the held time
        // is not played (single player: game time only runs a few ms behind the clock, never ahead).
        const XrTime period = fs.predictedDisplayPeriod;
        if (Cfg().holdSmooth && period > 0 && g_lastEndMs * 1e6 > (double)period && delta > period &&
            delta < 1000000000) {
            ++g_holdsSmoothed;
            g_holdNotPlayedMs += (double)(delta - period) * 1e-6;
            static int logs = 0;
            if (logs < 3) {
                ++logs;
                Log("VR: the runtime held an image %.1f ms - the game resumes where it stopped (%.1f ms not played "
                    "instead of a jump ahead; holdSmooth)", g_lastEndMs, (double)(delta - period) * 1e-6);
            }
            delta = period;
        }
        EngineSetVrFrameDelta((float)(delta * 1e-9));
    }
    // Refreshes the headset showed without a new image from the game (it repeated or
    // reprojected the previous one: a stutter), from the gaps between the predicted
    // display times; counted apart when the last xrEndFrame itself was held longer than
    // a refresh by the runtime (Virtual Desktop's stream). Gaps over 1 s (session
    // paused, level loading) are not counted.
    if (lastDisplay && fs.predictedDisplayPeriod > 0 && fs.predictedDisplayTime > lastDisplay &&
        fs.predictedDisplayTime - lastDisplay < 1000000000) {
        int missed = (int)std::max<XrTime>(
            0, (fs.predictedDisplayTime - lastDisplay + fs.predictedDisplayPeriod / 2) / fs.predictedDisplayPeriod - 1);
        g_displayMissed += missed;
        if (g_lastEndMs * 1e6 > (double)fs.predictedDisplayPeriod) g_displayMissedRt += missed;
    }
    g_lastEndMs = 0;
    lastDisplay = fs.predictedDisplayTime;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_frameState = fs;
    g_frameBegun = true;
    g_leftDone = false;
    g_viewsValid = XR_SUCCEEDED(r) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (g_viewsValid) {
        g_views[0] = views[0];
        g_views[1] = views[1];
    }
    if (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) g_headRaw = ToPose(hl.pose);
    // Recenter only on a real head position: right after the session starts,
    // Virtual Desktop reports the head at the floor (0, 0, 0) for a few frames.
    bool headTracked = (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                       (!g_stage || g_headRaw.p.y > 0.4f);
    LocateHands(fs.predictedDisplayTime);
    if (g_recenterPending && g_viewsValid && headTracked) ApplyRecenter();
    if (g_uiPlacePending && g_viewsValid && headTracked) PlaceUiPanel();
    // A grip pressed on one of our panels belongs to it: decided here, with the new controller
    // state and under the same lock, so the Lua thread never sees that press (VrHandleLuaRequest).
    UpdatePanelGrips();
}

void EndXrFrame(const XrCompositionLayerBaseHeader* const* layers, uint32_t count)
{
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = g_frameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = count;
    ei.layers = layers;
    {
        XrCallTimer t("xrEndFrame");
        double e0 = Now();
        XR_LOG_FAIL(xrEndFrame(g_session, &ei));
        g_lastEndMs = (Now() - e0) * 1000.0;
        // ([Performance] vdStats: every hold counted - the "blocked" lines stop after 60)
        Perf::NoteEndFrame(g_lastEndMs, g_frameState.predictedDisplayPeriod * 1e-6);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frameBegun = false;
    g_leftDone = false;
}

Pose ScreenPose()
{
    Pose local{ {}, { 0, g_refHeadY, -Cfg().screenDistance } };
    return Mul(g_recenter, local);
}

// The part of the virtual screen's image in use, from its top-left corner (pixels): the whole image, or its top
// 16:9 band while a paused level is drawn for a 16:9 screen ([Ecran] pauseWide; engine.cpp, WideState). Set when
// the image is written (OnPresent); the quad is screenWidth wide and has this shape.
int g_screenUseW = 0, g_screenUseH = 0;

void ScreenUse(int& w, int& h)
{
    w = g_screenUseW > 0 && g_screenUseW <= g_screenSc.w ? g_screenUseW : g_screenSc.w;
    h = g_screenUseH > 0 && g_screenUseH <= g_screenSc.h ? g_screenUseH : g_screenSc.h;
}

// The virtual screen's height in metres (its width is screenWidth).
float ScreenHeightM()
{
    int w = 0, h = 0;
    ScreenUse(w, h);
    return w > 0 ? Cfg().screenWidth * h / (float)w : 0.0f;
}

// Ray / quad intersection in app space. Returns true + pixel coords (+ tOut: metres along the ray).
bool RayQuad(const Pose& ray, const Pose& quad, float qw, float qh, float& u, float& v, float* tOut = nullptr)
{
    Vec3 dir = Rotate(ray.q, { 0, 0, -1 });
    Pose inv = Inverse(quad);
    Vec3 o = Rotate(inv.q, ray.p) + inv.p;
    Vec3 d = Rotate(inv.q, dir);
    if (std::abs(d.z) < 1e-5f) return false;
    float t = -o.z / d.z;
    if (t <= 0) return false;
    Vec3 hit = o + d * t;
    u = hit.x / qw + 0.5f;
    v = 0.5f - hit.y / qh;
    if (tOut) *tOut = t;   // (a unit direction and a rigid pose: metres)
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

// Ray / plane of a quad: coordinates in the quad (0..1 inside, beyond outside),
// also when the ray passes beside it (a drag on the inventory goes on past its
// edge). False if the ray does not reach the plane.
bool RayPlane(const Pose& ray, const Pose& quad, float qw, float qh, float& u, float& v, bool& inside,
              float* tOut = nullptr)
{
    Vec3 dir = Rotate(ray.q, { 0, 0, -1 });
    Pose inv = Inverse(quad);
    Vec3 o = Rotate(inv.q, ray.p) + inv.p;
    Vec3 d = Rotate(inv.q, dir);
    if (std::abs(d.z) < 1e-5f) return false;
    float t = -o.z / d.z;
    if (t <= 0) return false;
    Vec3 hit = o + d * t;
    u = hit.x / qw + 0.5f;
    v = 0.5f - hit.y / qh;
    inside = u >= 0 && u <= 1 && v >= 0 && v <= 1;
    if (tOut) *tOut = t;
    return true;
}

// --- Moving our floating panels with a grip (see "Floating panels" above) -------------------
// A grip press belongs to a panel only when it begins (rising edge) while that hand's ray is on
// it: on the settings / keys panel, or on the VR inventory pinned by R3 (not its brief display)
// while the player looks toward it - the same rule as its trigger. A press that began beside it
// stays the game's, even when the ray then crosses the panel. The off hand only with "both hands
// point at the menus" and its laser on (an invisible ray must not take anything), and never the
// inventory while that hand touches an object (Input::OffHandTouching: its grip grabs it). A
// second hand pressed on a panel already moved is kept from the game too (the object behind the
// panel is hidden by it), without taking it over. The press stays the panel's until that grip is
// seen released - also if the panel hides or closes meanwhile; never on a missing XR frame, an
// unfocused session (also when xrSyncActions already says so before the state-change event:
// g_actionsLive) or that controller's input inactive (g_handInput: asleep, disconnected, first
// sync after the headset's menu) - an inactive action reads 0, which is not a release. The input
// is then frozen: UpdatePanels only ends the hold after 0.5 s and a few presents without it, and
// ownership stays until the grip is really seen released.
struct PanelHit { int panel = kPanelNone; float u = 0, v = 0, t = 0, w = 0, h = 0, lx = 0, ly = 0; };

// A ray (recentred room space) on a mod's menu shown as a floating panel (engine.cpp, ModPanel): metres
// along it, the canvas point, and where: 1 = its content (+ EngineModPanel::kEdge), 2 = its frame band
// (up to kBand around the content: a grip there moves the panel, a click there is "beside the
// window"), 0 = elsewhere (or behind it).
int ModPanelZone(const Pose& aimRc, const EngineModPanel& mp, float& t, float& u, float& v)
{
    const Vec3 n = Rotate(mp.pose.q, { 0, 0, 1 }), d = Rotate(aimRc.q, { 0, 0, -1 });
    const float den = Dot(d, n);
    if (den > -0.05f || mp.k <= 0) return 0;
    t = Dot(mp.pose.p - aimRc.p, n) / den;
    if (!(t > 0)) return 0;
    const Vec3 l = Rotate(Conjugate(mp.pose.q), aimRc.p + d * t - mp.pose.p);
    u = mp.au + l.x / mp.k;
    v = mp.av - l.y / mp.k;
    const float x0 = mp.box ? mp.x0 : 0, y0 = mp.box ? mp.y0 : 0, x1 = mp.box ? mp.x1 : mp.cw, y1 = mp.box ? mp.y1 : mp.ch;
    auto in = [&](float m) { return u >= x0 - m && u <= x1 + m && v >= y0 - m && v <= y1 + m; };
    if (in(EngineModPanel::kEdge)) return 1;
    return mp.box && in(EngineModPanel::kBand) ? 2 : 0;
}

// A mod's menu on its floating panel this present (drawn over each eye's image, OnGamePresent): the
// pointing hand's grip would move it (its cursor turns blue: on the frame band, or anywhere on a
// window that never reads the right click), and a hand's ray is on it or moves it (the corners of
// the zone a grip moves it from are marked).
void ModPanelHighlight(bool& grabCursor, bool& corners)
{
    grabCursor = corners = false;
    EngineModPanel mp;
    if (!EngineGetModPanel(mp) || !mp.movable) return;   // (the revolver's ring: never moved by a grip)
    corners = mp.held >= 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Pose inv = Inverse(g_recenter);
    for (int i = 0; i < 2; ++i) {
        const HandState& h = g_handsRaw[i];
        if (!h.active) continue;
        float t = 0, u = 0, v = 0;
        const int zone = ModPanelZone(Mul(inv, h.aim), mp, t, u, v);
        if (zone) corners = true;
        if (i == mp.pointer && (zone == 2 || (zone == 1 && mp.rmbFree))) grabCursor = true;
    }
}

// The panel a grip pressed now by that hand would take (kPanelNone: none under its ray), and why
// that hand may not take it (*refused != nullptr: the press is then the game's). g_mutex held:
// UpdatePanelGrips, and VrHandleLuaRequest (a grip still rising there is kept from Lua).
// Inventory: only with a pose measured this present (g_invFocus is cleared while it is hidden -
// UpdatePanels - and a recenter re-places it: g_panelSnap), not where it was last shown.
// A mod's menu on its floating panel: only where the ray is on none of our panels (they are seen over
// it, whatever the distances). A grip on its frame band takes it; on its content only from the hand
// that does not point at it, or when the window never reads the right mouse button
// (EngineModPanel::rmbFree) - otherwise that grip stays the window's right click (input.cpp: the
// pointing hand's grip = right button), and nothing is taken.
PanelHit GripTarget(int i, const char** refused)
{
    *refused = nullptr;
    PanelHit hit;
    const HandState& h = g_handsRaw[i];
    if (!h.active) return hit;
    const Settings& s = Cfg();
    const float uw = 1.0f * s.uiScale, uh = uw * Ui::kHeight / (float)Ui::kWidth;
    const float mpp = Inventory::kMetersPerPixel * s.uiScale;
    const float iw = Inventory::PanelW() * mpp, ih = Inventory::PanelH() * mpp;
    float u = 0, v = 0, t = 0;
    bool inside = false;
    if (Ui::Visible() && RayQuad(h.aim, g_uiPose, uw, uh, u, v, &t))
        hit = { kPanelSettings, u, v, t, uw, uh };
    else if (Inventory::Visible() && Inventory::Interactive() && g_invFocus && !g_panelSnap &&
             RayPlane(h.aim, g_invPose, iw, ih, u, v, inside, &t) && inside)
        hit = { kPanelInventory, u, v, t, iw, ih };
    // (our panels are quad layers composited over the game image, where a mod's panel is drawn: under
    // one of them, even nearer along the ray, the mod's panel is hidden - the window's pointer, the
    // trigger, already gives way there: VrHandleLuaRequest)
    bool overOurs = hit.panel != kPanelNone;
    if (!overOurs && Inventory::Visible() && g_invFocus) {
        bool in = false;
        float iu = 0, iv = 0;
        overOurs = RayPlane(h.aim, g_invPose, iw, ih, iu, iv, in) && in;
    }
    EngineModPanel mp;
    bool trigger = false;
    // (the revolver's ring, not movable, is never taken: a grip there stays the window's right click or the game's)
    if (!overOurs && EngineGetModPanel(mp) && mp.movable) {
        float mt = 0, mu = 0, mv = 0;
        const int zone = ModPanelZone(Mul(Inverse(g_recenter), h.aim), mp, mt, mu, mv);
        if (zone) {
            const bool grab = zone == 2 || i != mp.pointer || mp.rmbFree;
            hit = grab ? PanelHit{ kPanelModWindow, mu, mv, mt, 0, 0, (mu - mp.au) * mp.k, -(mv - mp.av) * mp.k } : PanelHit{};
            trigger = grab && h.trigger > 0.35f;
        }
    }
    const int off = s.leftHanded ? 1 : 0;
    if (hit.panel != kPanelNone && i == off && !(s.offHandPointer && s.offLaser))
        *refused = "that hand does not point at the menus or has no laser";
    else if (hit.panel == kPanelInventory && i == off && Input::OffHandTouching())
        *refused = "that hand touches an object";
    else if (hit.panel == kPanelModWindow && i == off && Input::OffHandNear())
        *refused = "that hand touches an object (its grip takes it)";
    else if (trigger)
        *refused = "its trigger is held (a click or a drag in the window)";
    return hit;
}

void StartHold(int hand, const PanelHit& hit)
{
    g_hold.hand = hand;
    g_hold.panel = hit.panel;
    g_hold.dist = hit.t;
    // (the grab point from the panel's centre; a mod's window: from its anchor, metres - ModPanelZone)
    g_hold.lx = hit.panel == kPanelModWindow ? hit.lx : (hit.u - 0.5f) * hit.w;
    g_hold.ly = hit.panel == kPanelModWindow ? hit.ly : (0.5f - hit.v) * hit.h;
    g_holdHand = hand;
    g_trigMasked[hand] = true;
    if (hit.panel == kPanelModWindow) {
        EngineModPanel mp;
        g_hold.modSession = EngineGetModPanel(mp) ? mp.session : 0;
        EngineModPanelHold(hand);
    }
    if (Cfg().haptics) VrHaptic(hand, 0.35f, 0.04f);
    static int logs = 0;
    if (logs < 200) {
        ++logs;
        if (hit.panel == kPanelModWindow)
            Log("Panels: mod window taken by the %s grip at canvas %.0f, %.0f (%.3f m, %.3f m from its anchor), %.3f m along "
                "the ray (haptic %s)", hand ? "right" : "left", hit.u, hit.v, hit.lx, hit.ly, hit.t,
                Cfg().haptics ? "0.35 / 40 ms" : "off");
        else
            Log("Panels: %s taken by the %s grip at %.0f%%/%.0f%% of it, %.3f m along the ray (haptic %s)", PanelName(hit.panel),
                hand ? "right" : "left", hit.u * 100, hit.v * 100, hit.t, Cfg().haptics ? "0.35 / 40 ms" : "off");
    }
}

// The hand lets the panel go (grip released, panel hidden / closed, input lost). head: app space.
// Saves its place (ConfigSaveKeys: those keys only, on a worker thread). Takes no lock: called from
// BeginXrFrame (g_mutex held) and UpdatePanels (not held); the next UpdatePanels shows the result.
void EndHold(const Pose& head, const char* why)
{
    if (g_hold.hand < 0) return;
    const int hand = g_hold.hand;
    static int logs = 0;
    if (g_hold.panel == kPanelInventory) {
        // The anchor is on the neck (UpdateBodyAnchor, while held; here with the newest head: a lag
        // of the body's hysteresis would otherwise be saved in the place - the next R3 puts the
        // anchor on the neck), the panel stays where it is.
        const PanelPlace o0 = InvPlace();
        const Pose cur = PanelPose(kInvPanel, g_body.a, o0);
        g_body.a.pos = head.p - Rotate(head.q, kEyeFromNeck);
        g_body.followH = g_body.followV = false;
        const PanelPlace o = PlaceFromCentre(kInvPanel, g_body.a, cur.p, o0.yaw);
        Settings& s = Cfg();
        s.inventoryYaw = -o.yaw / kDeg;
        s.inventoryForward = o.fwd;
        s.inventoryDown = o.down;
        ConfigSaveKeys({ "inventoryYaw", "inventoryForward", "inventoryDown" });
        const Vec3 c = PanelPose(kInvPanel, g_body.a, o).p;   // (= cur.p, unless a limit moved it)
        if (logs++ < 200)
            Log("Panels: inventory placed %+.0f deg (+ = right), %.3f m ahead, %.3f m below the eyes, centre (%.3f %.3f %.3f)%s, "
                "tilt %.1f deg - %s, saved", s.inventoryYaw, o.fwd, o.down, c.x, c.y, c.z,
                Length(c - cur.p) > 0.005f ? " (moved by a limit)" : "", std::atan2(o.down, o.fwd) / kDeg, why);
    } else if (g_hold.panel == kPanelSettings) {
        // Where it opens next time, straight in front of the gaze: its distance and height from the
        // eyes now (the direction is not kept: it always opens where the player looks).
        const PanelPlace o = PlaceFromCentre(kUiPanel, { head.p, 0 }, g_uiPose.p, 0);
        Settings& s = Cfg();
        s.uiPanelDistance = o.fwd;
        s.uiPanelDown = o.down;
        ConfigSaveKeys({ "uiPanelDistance", "uiPanelDown" });
        if (logs++ < 200)
            Log("Panels: %s placed %.3f m ahead, %.3f m below the eyes, centre (%.3f %.3f %.3f), %.0f deg from the gaze, "
                "tilt %.1f deg - %s, saved", PanelName(kPanelSettings), o.fwd, o.down, g_uiPose.p.x, g_uiPose.p.y,
                g_uiPose.p.z, GazeAngleDeg(head, g_uiPose.p), std::atan2(o.down - kUiPanel.tiltBias, o.fwd) / kDeg, why);
    } else if (g_hold.panel == kPanelModWindow) {
        // (the engine keeps its place for that window and saves it: EngineModPanelRelease)
        EngineModPanelRelease();
        if (logs++ < 200) Log("Panels: mod window let go - %s", why);
    }
    if (Cfg().haptics) VrHaptic(hand, 0.15f, 0.02f);
    g_hold.hand = -1;
    g_hold.panel = kPanelNone;
    g_holdHand = -1;
    // (g_trigMasked[hand] stays until that trigger is let go: UpdatePanelGrips)
}

// BeginXrFrame (g_mutex held), with the controllers' new state.
void UpdatePanelGrips()
{
    // (no real input from the headset: everything waits, see UpdatePanels)
    if (!g_status.focused || !g_actionsLive) return;
    static int logs = 0;
    for (int i = 0; i < 2; ++i) {
        if (!g_handInput[i]) continue;   // (that controller's input inactive: its grip reads 0, not released)
        g_gripSeenAt[i] = Now();
        g_presentsUnseen[i] = 0;
        const HandState& h = g_handsRaw[i];
        const bool down = g_hold.gripDown[i] ? h.squeeze > 0.4f : h.squeeze > 0.6f;
        const bool pressed = down && !g_hold.gripDown[i];
        g_hold.gripDown[i] = down;
        if (!down) {
            if (g_hold.hand == i) EndHold(g_headRaw, "grip released");
            g_gripOwned[i] = false;
        } else if (pressed && h.active) {
            const char* refused = nullptr;
            const PanelHit hit = GripTarget(i, &refused);
            if (refused) {
                if (logs++ < 100)
                    Log("Panels: %s grip pressed on the %s, left to the game (%s)", i ? "right" : "left", PanelName(hit.panel), refused);
            } else if (hit.panel != kPanelNone) {
                g_gripOwned[i] = true;
                if (g_hold.hand < 0) StartHold(i, hit);
                else if (logs++ < 100)
                    Log("Panels: %s grip pressed on the %s while the %s grip moves the %s - kept from the game",
                        i ? "right" : "left", PanelName(hit.panel), g_hold.hand ? "right" : "left", PanelName(g_hold.panel));
            }
        }
        // (Lua: the panel's press, then that grip opening - between 0.4 and 0.25 main.lua would still
        // turn the tool copy and the free hand "ghost" - until it squeezes again: a new press)
        if (g_gripOwned[i]) {
            g_gripQuiet[i] = true;
            g_quietMin[i] = h.squeeze;
        } else if (g_gripQuiet[i]) {
            g_quietMin[i] = std::min(g_quietMin[i], h.squeeze);
            if (down || h.squeeze < 0.25f || h.squeeze > g_quietMin[i] + 0.1f) g_gripQuiet[i] = false;
        }
        // (the hand moving a panel: its trigger does nothing, until let go after the panel was released)
        if (g_hold.hand == i) g_trigMasked[i] = true;
        else if (g_trigMasked[i] && h.trigger < 0.15f) g_trigMasked[i] = false;
    }
}

// The settings / keys panel moved by the holding hand (present thread). Its anchor: the eyes now
// (it is room-fixed; its limits are distances from where the player is).
void MoveSettingsPanel(const Pose& head, const HandState& hand)
{
    const Anchor a{ head.p, 0 };
    Pose p = g_uiPose;
    PanelPlace o = PlaceFromCentre(kUiPanel, a, p.p, YawOf(p.q));
    p = DragPanel(kUiPanel, a, p, o, hand);
    std::lock_guard<std::mutex> lock(g_mutex);   // (the Lua thread reads it: OffLaserStop)
    g_uiPose = p;
}

// A mod's menu moved by the holding hand (present thread): ray drag like ours (the grab point stays
// under the ray), upright and facing the eyes, 0.35 to 3 m from them; the engine draws it there
// from the next stereo pair (EngineModPanelMove). In recentred room space, like the engine's panel.
void MoveModPanel(const Pose& head, const HandState& hand)
{
    EngineModPanel mp;
    if (!EngineGetModPanel(mp)) return;   // (closed: UpdatePanels ends the hold)
    const Pose inv = Inverse(g_recenter);
    const Pose aim = Mul(inv, hand.aim), eye = Mul(inv, head);
    const Vec3 g = aim.p + Rotate(aim.q, { 0, 0, -1 }) * g_hold.dist;
    Pose p = mp.pose;
    for (int pass = 0; pass < 2; ++pass) {
        Vec3 c = g - Rotate(p.q, { g_hold.lx, g_hold.ly, 0 });
        Vec3 r = c - eye.p;
        const float d = Length(r);
        if (d > 1e-3f) c = eye.p + r * (std::clamp(d, 0.35f, 3.0f) / d);
        r = eye.p - c;
        const float l = std::max(Length(r), 1e-4f);
        p.q = AxisAngle({ 0, 1, 0 }, std::atan2(r.x, r.z)) * AxisAngle({ 1, 0, 0 }, -std::asin(std::clamp(r.y / l, -1.0f, 1.0f)));
        p.p = c;
    }
    EngineModPanelMove(p);
}

// Once per present, before the panels are drawn and composited.
void UpdatePanels(float dt, bool placeInventory)
{
    Pose head;
    HandState holder;
    bool snap;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        head = g_headRaw;
        if (g_hold.hand >= 0) holder = g_handsRaw[g_hold.hand];
        snap = g_panelSnap;
        g_panelSnap = false;
    }
    // (no lock below: EndHold and the panels' updates take it themselves where needed)
    if (g_hold.hand >= 0) {
        // (input missing: 0.5 s AND a few presents without it - one long game frame, a hitch, is not)
        const int h = g_hold.hand;
        if (++g_presentsUnseen[h] > 4 && Now() - g_gripSeenAt[h] > 0.5)
            EndHold(head, "no input from that controller for 0.5 s (the grip stays the panel's until it is released)");
        else if (g_hold.panel == kPanelInventory && !Inventory::Interactive())
            EndHold(head, "inventory hidden");
        else if (g_hold.panel == kPanelSettings && !Ui::Visible())
            EndHold(head, "panel closed");
        else if (g_hold.panel == kPanelModWindow) {
            EngineModPanel mp;
            if (!EngineGetModPanel(mp) || mp.session != g_hold.modSession) EndHold(head, "window closed");
        }
    }
    const bool live = g_hold.hand >= 0 && holder.active;   // (tracking lost: the panel waits)
    if (Inventory::Visible()) {
        UpdateInventoryPanel(dt, placeInventory || snap, head, live && g_hold.panel == kPanelInventory ? &holder : nullptr);
    } else if (g_invFocus) {
        // (hidden: no grip, trigger or laser test on where it was last shown - the first ones after it
        // reappears would otherwise use that old place, before this present puts it in front again)
        std::lock_guard<std::mutex> lock(g_mutex);
        g_invFocus = false;
    }
    if (live && g_hold.panel == kPanelSettings) MoveSettingsPanel(head, holder);
    if (live && g_hold.panel == kPanelModWindow) MoveModPanel(head, holder);
    // (a frame around the panel while it is moved; a press the holding pointer hand's trigger began
    // just before its grip is dropped - Inventory::SetHeld, Ui::SetHeld)
    Inventory::SetHeld(g_hold.panel == kPanelInventory ? g_hold.hand : -1);
    Ui::SetHeld(g_hold.panel == kPanelSettings, g_hold.hand >= 0 && g_hold.hand == Input::PointerHand());
}

double g_lastPresent = 0, g_lastXrFrame = 0;

// Debug (simulated headset): virtual controllers driven by the keyboard, to
// test locomotion, turning, aiming and the button mapping without a Quest.
//   I/K/J/L left stick   U/O turn   T/G change tool   H right trigger
//   N right grip   M = A   B = B   V = X   Y left trigger   Z menu
void FakeHands(HandState& hl, HandState& hr)
{
    hl = HandState{};
    hr = HandState{};
    hl.active = hr.active = true;
    Quat down = AxisAngle({ 1, 0, 0 }, -15 * kDeg);
    // The right hand sways so the aim keeps changing (tests the camera against aim updates).
    Quat sway = AxisAngle({ 0, 1, 0 }, (float)std::sin(Now() * 0.7) * 20 * kDeg) * down;
    hr.aim = { sway, { 0.22f, 1.25f, -0.35f } };
    hr.grip = hr.aim;
    hl.aim = { down, { -0.22f, 1.25f, -0.35f } };
    hl.grip = hl.aim;
    if (GetForegroundWindow() != Input::GameWindow()) return;
    auto k = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    hl.stickY = (k('I') ? 1.0f : 0.0f) - (k('K') ? 1.0f : 0.0f);
    hl.stickX = (k('L') ? 1.0f : 0.0f) - (k('J') ? 1.0f : 0.0f);
    hr.stickX = (k('O') ? 1.0f : 0.0f) - (k('U') ? 1.0f : 0.0f);
    hr.stickY = (k('T') ? 1.0f : 0.0f) - (k('G') ? 1.0f : 0.0f);
    hr.trigger = k('H') ? 1.0f : 0.0f;
    hr.squeeze = k('N') ? 1.0f : 0.0f;
    hl.trigger = k('Y') ? 1.0f : 0.0f;
    hr.primary = k('M');
    hr.secondary = k('B');
    hl.primary = k('V');
    hl.menu = k('Z');
}

// ---------------------------------------------------------------------------
// Debug: synchronisation test. The Lua mod draws a red (left) / blue (right)
// marker in the top-left corner and points the camera at the sky (left) or the
// ground (right). We read back the presented image and check that it matches
// the eye we handed out for that frame.
// ---------------------------------------------------------------------------
ID3D12Resource* g_readback[4] = {};
int g_readbackEye[4] = { -1, -1, -1, -1 };
struct SyncStats { int samples = 0, markerOk = 0, markerBad = 0, markerNone = 0; double lum[2] = { 0, 0 }; int n[2] = { 0, 0 }; } g_sync;

void SyncTestEvaluate(int slot)
{
    if (g_readbackEye[slot] < 0 || !g_readback[slot]) return;
    BYTE* p = nullptr;
    D3D12_RANGE r = { 0, 1024 };
    if (FAILED(g_readback[slot]->Map(0, &r, (void**)&p))) return;
    BYTE* marker = p;
    BYTE* center = p + 512;
    int eye = g_readbackEye[slot];
    int markerEye = (marker[0] > 180 && marker[2] < 80) ? 0 : (marker[2] > 180 && marker[0] < 80) ? 1 : -1;
    double lum = 0.299 * center[0] + 0.587 * center[1] + 0.114 * center[2];
    D3D12_RANGE none = { 0, 0 };
    g_readback[slot]->Unmap(0, &none);
    g_readbackEye[slot] = -1;

    static int verbose = 0;
    if (verbose < 24) {
        ++verbose;
        Log("SyncTest sample: eye=%d marker=%d lum=%.0f", eye, markerEye, lum);
    }
    g_sync.samples++;
    if (markerEye < 0) g_sync.markerNone++;
    else if (markerEye == eye) g_sync.markerOk++;
    else g_sync.markerBad++;
    g_sync.lum[eye] += lum;
    g_sync.n[eye]++;
    if (g_sync.samples % 240 == 0) {
        Log("SyncTest: %d frames, marker ok=%d wrong=%d none=%d | centre luminance: left(sky)=%.0f right(ground)=%.0f",
            g_sync.samples, g_sync.markerOk, g_sync.markerBad, g_sync.markerNone,
            g_sync.n[0] ? g_sync.lum[0] / g_sync.n[0] : 0, g_sync.n[1] ? g_sync.lum[1] / g_sync.n[1] : 0);
        g_sync = SyncStats{};
    }
}

// ---------------------------------------------------------------------------
// Debug: dump 8 consecutive presented frames to BMP files (game folder) with
// the eye each frame was rendered for. Lets us look at what really went out.
// ---------------------------------------------------------------------------
ID3D12Resource* g_dumpBuf[8] = {};
int g_dumpEye[8];
int g_dumpCount = -1;   // -1 idle, 0..8 capturing
UINT g_dumpPitch = 0;

void DumpCapture(ID3D12GraphicsCommandList* cl, ID3D12Resource* bb, DXGI_FORMAT fmt, int eye)
{
    if (g_dumpCount < 0 || g_dumpCount >= 8 || fmt != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    int i = g_dumpCount++;
    g_dumpPitch = (g_bbW * 4 + 255) & ~255u;
    if (!g_dumpBuf[i]) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)g_dumpPitch * g_bbH;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&g_dumpBuf[i]));
    }
    g_dumpEye[i] = eye;
    Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = g_dumpBuf[i];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { fmt, (UINT)g_bbW, (UINT)g_bbH, 1, g_dumpPitch };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void DumpWrite()
{
    if (g_dumpCount != 8) return;
    g_dumpCount = -2; // done
    Render::WaitIdle();
    for (int i = 0; i < 8; ++i) {
        BYTE* p = nullptr;
        D3D12_RANGE r = { 0, (SIZE_T)g_dumpPitch * g_bbH };
        if (!g_dumpBuf[i] || FAILED(g_dumpBuf[i]->Map(0, &r, (void**)&p))) continue;
        // Half resolution 24-bit BMP.
        int w = g_bbW / 2, h = g_bbH / 2, row = (w * 3 + 3) & ~3;
        std::vector<BYTE> img((size_t)row * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                BYTE* s = p + (size_t)(y * 2) * g_dumpPitch + x * 2 * 4;
                BYTE* d = &img[(size_t)(h - 1 - y) * row + x * 3];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
            }
        D3D12_RANGE none = { 0, 0 };
        g_dumpBuf[i]->Unmap(0, &none);
        char name[MAX_PATH];
        snprintf(name, sizeof(name), "%lsTDVR_frame%d_eye%d.bmp", ModDirectory().c_str(), i, g_dumpEye[i]);
        FILE* f = fopen(name, "wb");
        if (!f) continue;
        BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(54 + img.size()), 0, 0, 54 };
        BITMAPINFOHEADER ih = { sizeof(ih), w, h, 1, 24, BI_RGB, (DWORD)img.size(), 2835, 2835, 0, 0 };
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(img.data(), 1, img.size(), f);
        fclose(f);
    }
    Log("Debug: 8 frames dumped next to the game exe (TDVR_frame*.bmp)");
}

void SyncTestCapture(ID3D12GraphicsCommandList* cl, ID3D12Resource* bb, DXGI_FORMAT fmt, int eye)
{
    int slot = Render::FrameSlot();
    SyncTestEvaluate(slot); // the GPU finished this slot's previous frame
    if (eye < 0 || fmt != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    if (!g_readback[slot]) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 1024;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&g_readback[slot]))))
            return;
    }
    Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = g_readback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { fmt, 1, 1, 1, 256 };
    D3D12_BOX box = { 2, 2, 0, 3, 3, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    dst.PlacedFootprint.Offset = 512;
    UINT cx = g_bbW / 2, cy = g_bbH / 2;
    D3D12_BOX cbox = { cx, cy, 0, cx + 1, cy + 1, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &cbox);
    Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_readbackEye[slot] = eye;
}

// ---------------------------------------------------------------------------
// Hidden-area fill ([Performance] hiddenAreaFill / hiddenAreaGuard; TeardownVR_perf.txt
// "hiddenAreaFill=N")
// ---------------------------------------------------------------------------
// An eye image is a rectangle, but the lenses show a rounded part of it: the runtime's lens mask
// (XR_KHR_visibility_mask, its "hidden" triangle mesh) says what is never seen - on a Quest,
// mostly the four corners. Virtual Desktop still encodes those pixels and sends them over Wi-Fi.
// Cleared to black just before the image is released, they cost its encoder almost nothing: with
// a quality-driven rate control the frame gets smaller (less airtime); with a constant bitrate
// the same bits go to the visible part. What the player sees stays the game's image: only whole
// 16-pixel blocks (an H.264 macroblock) are cleared, each sample of which (corners, centre) is
// hidden, and only those at least hiddenAreaGuard degrees from the nearest block with a visible
// sample - the headset shows a frame re-projected to a newer head pose (and repeats a late one),
// which brings a little of the hidden border to the lens edge: that must still be the game's
// image. The mask is in the view's z = -1 plane (+x right, +y up), mapped through the field of
// view the image covers (pv.fov: with fovCrop the headset's whole field), as the runtime maps the image itself.
// Modes: 1 = the runtime's mask; 2 = TEST shape for a runtime without a mask (the mock: an
// assumed rounded square, never the headset's real edge - not for play); 3 = TEST in the headset:
// mode 1 drawn pink with no margin (pink seen while looking around = a wrong mask: report it).

// p inside triangle abc, either winding (on an edge: inside - the mask's triangles share edges).
bool InTriangle(const XrVector2f& a, const XrVector2f& b, const XrVector2f& c, float x, float y)
{
    const float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
    const float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
    const float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

// Both eyes' hidden meshes from the runtime (the two-call idiom); none = an empty list.
void FetchHiddenMeshes()
{
    g_hidden.fetched = true;
    ++g_hidden.gen;
    for (int e = 0; e < 2; ++e) {
        std::vector<XrVector2f>& out = g_hidden.tri[e];
        out.clear();
        if (!pGetVisMask || !g_session) continue;
        XrVisibilityMaskKHR m = { XR_TYPE_VISIBILITY_MASK_KHR };
        XrResult r = pGetVisMask(g_session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)e,
                                 XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
        if (XR_FAILED(r) || !m.vertexCountOutput || m.indexCountOutput < 3) {
            Log("XR: no lens mask for eye %d (%s, %u vertices)", e, ResultStr(r), m.vertexCountOutput);
            continue;
        }
        std::vector<XrVector2f> v(m.vertexCountOutput);
        std::vector<uint32_t> idx(m.indexCountOutput);
        m.vertexCapacityInput = (uint32_t)v.size();
        m.vertices = v.data();
        m.indexCapacityInput = (uint32_t)idx.size();
        m.indices = idx.data();
        r = pGetVisMask(g_session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)e,
                        XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
        if (XR_FAILED(r)) {
            Log("XR: lens mask for eye %d: %s", e, ResultStr(r));
            continue;
        }
        const uint32_t nv = std::min<uint32_t>(m.vertexCountOutput, (uint32_t)v.size());
        const uint32_t ni = std::min<uint32_t>(m.indexCountOutput, (uint32_t)idx.size());
        for (uint32_t i = 0; i + 2 < ni; i += 3)
            if (idx[i] < nv && idx[i + 1] < nv && idx[i + 2] < nv) {
                out.push_back(v[idx[i]]);
                out.push_back(v[idx[i + 1]]);
                out.push_back(v[idx[i + 2]]);
            }
    }
}

// The rectangles of one eye's image (w x h pixels, drawn with the field of view fov; the headset's
// own field for that eye: view) that may be cleared. A block is out of view when its 5 samples
// are (in the mask, or outside the eye's field); it is cleared when its Chebyshev distance d (in
// blocks) to the nearest block with a visible sample covers the margin there: an angle a moves a
// point of the z = -1 plane at distance r from the view axis by at most a * (1 + r^2) (radially),
// and blocks d apart (centre to centre) are at least d - 1 blocks apart edge to edge. Rows of
// cleared blocks are merged into rectangles.
void BuildHiddenRects(int eye, int w, int h, const XrFovf& fov, const XrFovf& view, int mode, float guardDeg)
{
    const double t0 = Now();
    HiddenArea& ha = g_hidden;
    constexpr int kB = 16;
    const int cols = (w + kB - 1) / kB, rows = (h + kB - 1) / kB;
    const float tl = std::tan(-fov.angleLeft), tr = std::tan(fov.angleRight);
    const float tu = std::tan(fov.angleUp), td = std::tan(-fov.angleDown);
    const float vl = std::tan(-view.angleLeft), vr = std::tan(view.angleRight);
    const float vu = std::tan(view.angleUp), vd = std::tan(-view.angleDown);
    const float sx = (tl + tr) / w, sy = (tu + td) / h;   // plane units per pixel
    const std::vector<XrVector2f>& tris = ha.tri[eye];
    // (mode 2, the test shape: |x|^4 + |y|^4 > 1 in the eye's own field, about 7.6% of it)
    const float cx = (vr - vl) * 0.5f, cy = (vu - vd) * 0.5f, ax = (vl + vr) * 0.5f, ay = (vu + vd) * 0.5f;
    struct TriSpan { size_t i; float x0, x1; };
    std::vector<TriSpan> rowTris;   // the triangles that reach the block row, with their x range
    auto hidden = [&](float x, float y) {
        if (x < -vl || x > vr || y > vu || y < -vd) return true;   // outside the headset's field for this eye
        if (mode == 2) {
            const float nx = (x - cx) / ax, ny = (y - cy) / ay;
            return nx * nx * nx * nx + ny * ny * ny * ny > 1.0f;
        }
        for (const TriSpan& t : rowTris)
            if (x >= t.x0 && x <= t.x1 && InTriangle(tris[t.i], tris[t.i + 1], tris[t.i + 2], x, y)) return true;
        return false;
    };
    constexpr int kFar = 1 << 20;
    std::vector<int> dist((size_t)cols * rows);
    auto at = [&](int r, int c) -> int& { return dist[(size_t)r * cols + c]; };
    int outBlocks = 0;
    for (int r = 0; r < rows; ++r) {
        const float y0 = tu - r * kB * sy, y1 = tu - std::min(h, (r + 1) * kB) * sy;   // top, bottom (+y up)
        rowTris.clear();
        if (mode != 2)
            for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                const float lo = std::min({ tris[i].y, tris[i + 1].y, tris[i + 2].y });
                const float hi = std::max({ tris[i].y, tris[i + 1].y, tris[i + 2].y });
                if (hi >= y1 && lo <= y0)
                    rowTris.push_back({ i, std::min({ tris[i].x, tris[i + 1].x, tris[i + 2].x }),
                                        std::max({ tris[i].x, tris[i + 1].x, tris[i + 2].x }) });
            }
        for (int c = 0; c < cols; ++c) {
            const float x0 = -tl + c * kB * sx, x1 = -tl + std::min(w, (c + 1) * kB) * sx;
            const bool out = hidden(x0, y0) && hidden(x1, y0) && hidden(x0, y1) && hidden(x1, y1) &&
                             hidden((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
            at(r, c) = out ? kFar : 0;
            outBlocks += out ? 1 : 0;
        }
    }
    // Chebyshev distance to the nearest block with a visible sample (two passes; past the image's
    // edge counts as hidden: the headset shows nothing there either)
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            int& d = at(r, c);
            if (c > 0) d = std::min(d, at(r, c - 1) + 1);
            if (r > 0) {
                d = std::min(d, at(r - 1, c) + 1);
                if (c > 0) d = std::min(d, at(r - 1, c - 1) + 1);
                if (c + 1 < cols) d = std::min(d, at(r - 1, c + 1) + 1);
            }
        }
    for (int r = rows - 1; r >= 0; --r)
        for (int c = cols - 1; c >= 0; --c) {
            int& d = at(r, c);
            if (c + 1 < cols) d = std::min(d, at(r, c + 1) + 1);
            if (r + 1 < rows) {
                d = std::min(d, at(r + 1, c) + 1);
                if (c + 1 < cols) d = std::min(d, at(r + 1, c + 1) + 1);
                if (c > 0) d = std::min(d, at(r + 1, c - 1) + 1);
            }
        }
    // (one block more than the margin: a block whose 5 samples are hidden may still hold a sliver of
    // the visible part between them; the pink test, mode 3, keeps none)
    const float guard = std::max(0.0f, guardDeg) * kDeg, minBlocks = mode == 3 ? 1.0f : 2.0f;
    const float pxPerUnit = std::max(w / (tl + tr), h / (tu + td));
    std::vector<uint8_t> fill((size_t)cols * rows);
    long long filledPx = 0, maskPx = 0;   // cleared pixels; of them, inside the eye's field (from the mask)
    bool central = false;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const int d = at(r, c);
            if (d == 0) continue;
            const float x = -tl + (c + 0.5f) * kB * sx, y = tu - (r + 0.5f) * kB * sy;
            const float need = guard * (1.0f + x * x + y * y) * pxPerUnit / kB + minBlocks;
            if ((float)d < need) continue;
            fill[(size_t)r * cols + c] = 1;
            const long long px = (long long)(std::min(w, (c + 1) * kB) - c * kB) * (std::min(h, (r + 1) * kB) - r * kB);
            filledPx += px;
            if (x >= -vl && x <= vr && y <= vu && y >= -vd) maskPx += px;
            const float u = (c + 0.5f) * kB / w, v = (r + 0.5f) * kB / h;
            if (std::abs(u - 0.5f) < 0.25f && std::abs(v - 0.5f) < 0.25f) central = true;
        }
    std::vector<D3D12_RECT> rects;
    std::vector<size_t> prev, cur;   // the previous / this row's rectangles
    for (int r = 0; r < rows; ++r) {
        cur.clear();
        for (int c = 0; c < cols;) {
            if (!fill[(size_t)r * cols + c]) {
                ++c;
                continue;
            }
            int c1 = c;
            while (c1 < cols && fill[(size_t)r * cols + c1]) ++c1;
            const LONG L = c * kB, R = std::min(w, c1 * kB), T = r * kB, Bt = std::min(h, (r + 1) * kB);
            size_t k = rects.size();
            for (size_t p : prev)
                if (rects[p].left == L && rects[p].right == R && rects[p].bottom == T) {
                    k = p;
                    break;
                }
            if (k < rects.size()) rects[k].bottom = Bt;
            else rects.push_back({ L, T, R, Bt });
            cur.push_back(k);
            c = c1;
        }
        prev.swap(cur);
    }
    // Sanity: a mask mapped wrong (or a runtime's odd one) must never black out the view's middle, nor
    // much of the eye's field (what lies outside that field - an image wider than the headset's field,
    // the mock at 1920x1080 - is out of view by the runtime's own field of view: not counted here).
    const double wouldPct = 100.0 * (double)filledPx / ((double)w * h), maskPct = 100.0 * (double)maskPx / ((double)w * h);
    const char* refused = central ? "a block near the image's centre" : maskPct > 35.0 ? "more than 35% of the eye's field" : nullptr;
    if (refused) rects.clear();
    const double fillPct = refused ? 0.0 : wouldPct;
    ha.rects[eye] = std::move(rects);
    ha.built[eye] = true;
    ha.w[eye] = w;
    ha.h[eye] = h;
    ha.mode[eye] = mode;
    ha.guard[eye] = guardDeg;
    ha.fov[eye] = fov;
    ha.view[eye] = view;
    ha.builtGen[eye] = ha.gen;
    ha.fillPct[eye] = fillPct;
    if (ha.builds < 12 || refused) {
        ++ha.builds;
        char what[96];
        if (mode == 2) snprintf(what, sizeof(what), "TEST shape (a rounded square, not the headset's lenses)");
        else if (tris.empty()) snprintf(what, sizeof(what), "no lens mask from the runtime (only outside the eye's field)");
        else snprintf(what, sizeof(what), "lens mask of %zu triangles", tris.size() / 3);
        Log("XR: hidden-area fill, eye %d (%dx%d): %s; out of view %.1f%% of the image, %.1f%% cleared in %zu "
            "rectangles (%.1f%% inside the eye's field; margin %.1f deg, %.1f ms)%s%s", eye, w, h, what,
            100.0 * outBlocks / ((double)cols * rows), wouldPct, refused ? (size_t)0 : ha.rects[eye].size(), maskPct, guardDeg,
            (Now() - t0) * 1000.0, refused ? " - NOTHING CLEARED: " : "", refused ? refused : "");
    }
}

// Before this eye's image is released (frame A: left, frame B: right): its cleared rectangles, one
// ClearRenderTargetView with the rectangle list (no shader, nothing else drawn, ~microseconds).
void FillHidden(ID3D12GraphicsCommandList* cl, ID3D12Resource* img, int eye)
{
    HiddenArea& ha = g_hidden;
    const int ctl = FrameB::FileHiddenArea();   // (TeardownVR_perf.txt "hiddenAreaFill=N": A/B tests)
    const int asked = std::clamp(ctl >= 0 ? ctl : Cfg().hiddenAreaFill, 0, 3);
    // (the TEST shape is a guess, never a headset's lenses: on the mock runtime only - a control file
    // left over from a mock test must not black out a real headset's corners)
    const bool noTest = asked == 2 && g_status.runtime.find("Mock") == std::string::npos;
    const int mode = noTest ? 0 : asked;
    if (mode != ha.loggedMode) {
        // (a line when it turns on and when it goes back off; nothing while it stays off)
        if (mode || ha.loggedMode > 0 || noTest) {
            static const char* const kNames[] = { "off", "the runtime's lens mask, black",
                                                  "TEST shape (mock only: not the headset's lenses)",
                                                  "TEST: the runtime's lens mask in pink, no margin" };
            Log("XR: hidden-area fill %d: %s%s%s", mode, kNames[mode], ctl >= 0 ? " (TeardownVR_perf.txt)" : "",
                noTest ? " - hiddenAreaFill=2 (TEST shape) refused: not the mock runtime" : "");
        }
        ha.loggedMode = mode;
    }
    ++ha.images;
    if (!mode) return;
    if ((mode == 1 || mode == 3) && !ha.fetched) {
        FetchHiddenMeshes();
        if (!pGetVisMask)
            Log("XR: no lens mask (XR_KHR_visibility_mask not enabled at start - hiddenAreaFill neither in the ini nor in "
                "TeardownVR_perf.txt then - or not offered by the runtime): only what lies outside each eye's field is cleared");
    }
    XrFovf view;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        view = g_views[eye].fov;
    }
    if (view.angleRight - view.angleLeft < 0.1f || view.angleUp - view.angleDown < 0.1f) return;   // (no view located yet)
    const XrFovf& fov = g_submitFov[eye];   // (what the image covers: pv.fov)
    if (fov.angleRight - fov.angleLeft < 0.1f || fov.angleUp - fov.angleDown < 0.1f) return;   // (not drawn with one yet)
    const int w = g_eyeSc[eye].w, h = g_eyeSc[eye].h;
    const float iniGuard = Cfg().hiddenAreaGuard;   // (NaN or negative: the default, never "no margin")
    const float guard = mode == 3 ? 0.0f : iniGuard >= 0.0f ? std::min(iniGuard, 10.0f) : 2.0f;
    auto same = [](const XrFovf& a, const XrFovf& b) {
        return std::abs(a.angleLeft - b.angleLeft) < 1e-4f && std::abs(a.angleRight - b.angleRight) < 1e-4f &&
               std::abs(a.angleUp - b.angleUp) < 1e-4f && std::abs(a.angleDown - b.angleDown) < 1e-4f;
    };
    if (ha.off) return;
    if (!ha.built[eye] || ha.w[eye] != w || ha.h[eye] != h || ha.mode[eye] != mode || ha.guard[eye] != guard ||
        ha.builtGen[eye] != ha.gen || !same(ha.fov[eye], fov) || !same(ha.view[eye], view)) {
        // (a field of view that changes at every frame - an eye-tracked runtime - would rebuild at every
        // image, a few ms each: the fill then stops for this session)
        const double now = Now();
        ha.quick[eye] = now - ha.lastBuild[eye] < 2.0 ? ha.quick[eye] + 1 : 0;
        ha.lastBuild[eye] = now;
        if (ha.quick[eye] > 5) {
            ha.off = true;
            Log("XR: hidden-area fill stopped for this session: eye %d's field of view keeps changing", eye);
            return;
        }
        BuildHiddenRects(eye, w, h, fov, view, mode, guard);
    }
    const std::vector<D3D12_RECT>& rects = ha.rects[eye];
    if (rects.empty()) return;
    static const float kBlack[4] = { 0, 0, 0, 1 }, kPink[4] = { 1, 0, 1, 1 };
    cl->ClearRenderTargetView(Render::TransientRtv(img, g_colorFormat), mode == 3 ? kPink : kBlack, (UINT)rects.size(),
                              rects.data());
    ha.pctSum += ha.fillPct[eye];
}

// fovCrop ([Affichage] fovCoverage < 1): each eye image covers the headset's whole field (g_submitFov, what the
// runtime is told: the field of view used at 100 %), the game drew only part of it (g_renderFov). Where that part
// lies in the image: both are the same z = -1 plane mapped linearly, so it keeps its place and size - nothing is
// magnified whatever the runtime does with pv.fov (Virtual Desktop showed a narrower pv.fov stretched over the
// whole view). The whole image when both are the same (100 %, or fovCrop off).
D3D12_RECT CropRect(int eye, LONG w, LONG h)
{
    const D3D12_RECT whole = { 0, 0, w, h };
    const XrFovf& f = g_submitFov[eye];
    const XrFovf& r = g_renderFov[eye];
    const float fl = std::tan(-f.angleLeft), fr = std::tan(f.angleRight), fu = std::tan(f.angleUp), fd = std::tan(-f.angleDown);
    const float rl = std::tan(-r.angleLeft), rr = std::tan(r.angleRight), ru = std::tan(r.angleUp), rd = std::tan(-r.angleDown);
    if (!(fl + fr > 0.01f && fu + fd > 0.01f && rl + rr > 0.01f && ru + rd > 0.01f)) return whole;   // (none yet)
    auto at = [](float t, LONG n) { return std::clamp<LONG>((LONG)(t * n + 0.5f), 0, n); };
    D3D12_RECT c = { at((fl - rl) / (fl + fr), w), at((fu - ru) / (fu + fd), h), at((fl + rr) / (fl + fr), w),
                     at((fu + rd) / (fu + fd), h) };
    // (within 2 px of the game's image size - the coverage the render size was chosen with: exactly that size,
    // same centre, so the image is copied pixel for pixel; 1 px narrower would resample all of it, a half-pixel
    // blur across the middle of the view)
    auto snap = [](LONG& lo, LONG& hi, LONG want, LONG n) {
        const LONG d = hi - lo - want;
        if (want < 16 || want > n || d > 2 || d < -2) return;
        lo = std::clamp<LONG>((lo + hi - want) / 2, 0, n - want);
        hi = lo + want;
    };
    snap(c.left, c.right, (LONG)(g_bbW * Cfg().eyeResScale), w);
    snap(c.top, c.bottom, (LONG)(g_bbH * Cfg().eyeResScale), h);
    if (c.right - c.left < 16 || c.bottom - c.top < 16) return whole;   // (never a sliver)
    static D3D12_RECT logged[2] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
    D3D12_RECT& l = logged[eye & 1];
    if (l.left != c.left || l.top != c.top || l.right != c.right || l.bottom != c.bottom) {
        l = c;
        if (c.left || c.top || c.right != w || c.bottom != h)
            Log("XR: eye %d image %ldx%ld covers the headset's whole field; the game's image (%dx%d) is drawn at x %ld..%ld "
                "y %ld..%ld, black around (fovCoverage %.0f%%, fovCrop)", eye, w, h, g_bbW, g_bbH, c.left, c.right, c.top,
                c.bottom, FovCoverage() * 100.0f);
        else
            Log("XR: eye %d image %ldx%ld: the game's image fills it (whole field)", eye, w, h);
    }
    return c;
}

// The part of an eye image outside the game's image (CropRect): black, up to 4 rectangles in one clear (the
// swapchain's images keep older frames' pixels).
void ClearOutside(ID3D12GraphicsCommandList* cl, ID3D12Resource* img, const D3D12_RECT& c, LONG w, LONG h)
{
    D3D12_RECT band[4];
    UINT n = 0;
    if (c.top > 0) band[n++] = { 0, 0, w, c.top };
    if (c.bottom < h) band[n++] = { 0, c.bottom, w, h };
    if (c.left > 0) band[n++] = { 0, c.top, c.left, c.bottom };
    if (c.right < w) band[n++] = { c.right, c.top, w, c.bottom };
    static const float kBlack[4] = { 0, 0, 0, 1 };
    if (n) cl->ClearRenderTargetView(Render::TransientRtv(img, g_colorFormat), kBlack, n, band);
}

// Mean share of the eye images cleared since the last call (percent; 0 = off) - "Stereo frames" line.
double HiddenFillPct()
{
    const double p = g_hidden.images ? g_hidden.pctSum / g_hidden.images : 0.0;
    g_hidden.pctSum = 0;
    g_hidden.images = 0;
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Present hook entry points
// ---------------------------------------------------------------------------
// Is a headset in use? Checked once when the game starts (before it creates its
// window, so the window already gets the VR size), then by the live session.
static int g_headsetAtStartup = -1;   // the start-up check: -1 not made, 0 not connected, 1 connected

bool HeadsetInUse()
{
    if (g_running || Cfg().debugFakeHmd) return true;
    static std::once_flag once;
    static bool atStartup = false;
    std::call_once(once, [] {
        // (the game's runtime is chosen here: headsetRuntime 0, the one that has the headset; otherwise the one
        // set in the settings, only asked as before)
        atStartup = RuntimeStartup();
        Log("Render resolution: headset %s when the game started", atStartup ? "connected" : "not connected");
        g_headsetAtStartup = atStartup ? 1 : 0;
    });
    return atStartup;
}

bool VrRenderSizeLate()
{
    if (!g_running || g_headsetAtStartup != 0 || Cfg().debugFakeHmd) return false;
    // (only when a restart would give more: the preset "PC screen" has no VR size, and a back buffer
    // already as tall as the preset is not low)
    int pw = 0, ph = 0;
    RenderPresetSize(Cfg().renderPreset, pw, ph, RenderCoverage());
    return ph > 0 && g_status.backbufferH > 0 && g_status.backbufferH < ph;
}

// fovCrop: the share of the field the game's render size is chosen with - read once, when the game starts (the
// size is only chosen then, see UpdateRenderResolution): a change in game moves the black border at once
// (CropRect), the render size follows at the next start.
static float StartCoverage()
{
    static const float cov = RenderCoverage();
    return cov;
}

void GetSwapchainSizeOverride(UINT& w, UINT& h)
{
    w = h = 0;
    const float cov = StartCoverage();   // (latched at the game's first query, headset or not)
    if (!HeadsetInUse()) return;
    int pw = 0, ph = 0;
    RenderPresetSize(Cfg().renderPreset, pw, ph, cov);
    w = (UINT)pw;
    h = (UINT)ph;
}

// Called every frame: remembers the shape of the headset's field of view so
// that the next game start renders at matching proportions. The render size is
// only chosen when the game starts (re-applying it in game broke the image: the
// game re-read a size from another source than the one we report).
void UpdateRenderResolution()
{
    if (!g_running || !g_viewsValid) return;
    float needH = 0, needV = 0;
    for (const XrView& v : g_views) {
        if (Cfg().offCenter) {
            // Exact field of view per eye: width = left + right, height = up + down.
            needH = std::max(needH, (std::tan(-v.fov.angleLeft) + std::tan(v.fov.angleRight)) * 0.5f);
            needV = std::max(needV, (std::tan(v.fov.angleUp) + std::tan(-v.fov.angleDown)) * 0.5f);
        } else {
            needH = std::max({ needH, std::tan(-v.fov.angleLeft), std::tan(v.fov.angleRight) });
            needV = std::max({ needV, std::tan(v.fov.angleUp), std::tan(-v.fov.angleDown) });
        }
    }
    float aspect = needV > 0.1f ? needH / needV : 0;
    if (aspect > 0.5f && aspect < 2.5f && std::abs(aspect - Cfg().vrAspect) > 0.01f) {
        Log("Render resolution: headset field of view aspect %.3f (used from the next game start)", aspect);
        Cfg().vrAspect = aspect;
        ConfigSave();
    }
}

void OnGameBeforeResize(IDXGISwapChain3*)
{
    if (Render::Ready()) Render::WaitIdle();
}

void OnGameAfterResize(IDXGISwapChain3*) {}

void OnGamePresent(IDXGISwapChain3* sc, ID3D12CommandQueue* queue, UINT& sync, UINT& flags)
{
    static bool inited = false;
    if (!inited) {
        inited = true;
        queue->GetDevice(IID_PPV_ARGS(&g_device));
        g_queue = queue;
        HWND hwnd = nullptr;
        sc->GetHwnd(&hwnd);
        DXGI_SWAP_CHAIN_DESC1 d;
        sc->GetDesc1(&d);
        Log("First present: hwnd=%p %ux%u fmt=%d buffers=%u", hwnd, d.Width, d.Height, d.Format, d.BufferCount);
        {
            // fovCrop: a back buffer sized for part of the headset's field (GetSwapchainSizeOverride): the eye
            // images cover the whole field at its pixel density (EnsureSwapchains).
            UINT ow = 0, oh = 0;
            GetSwapchainSizeOverride(ow, oh);
            const float c = StartCoverage();
            if (c < 1.0f && oh && d.Height + 8 >= oh && d.Height <= oh + 8) {
                g_cropBase = c;
                g_status.cropBase = c;
                Log("Render resolution: %ux%u = %.0f%% of the headset's field each way (fovCoverage, fovCrop): the eye "
                    "images cover the whole field at this sharpness, black around the game's image", d.Width, d.Height,
                    c * 100.0f);
            }
        }
        EngineInit();
        if (!Render::Init(g_device, queue)) Log("Render init FAILED");
        Input::Init(hwnd);
        Ui::Init(hwnd);
        // Other programs using the game's graphics card (warning for the player).
        if (g_device) {
            LUID luid = g_device->GetAdapterLuid();
            GpuWatch::Start(((uint64_t)(uint32_t)luid.HighPart << 32) | luid.LowPart);
        }
        // Overlays hooked into every Present (Steam, NVIDIA): logged, and one line in the settings panel.
        Perf::CheckOverlays();
    }
    if (!Render::Ready()) return;
    HangDetectorTick(g_device);

    double now = Now();
    float dt = g_lastPresent > 0 ? float(now - g_lastPresent) : 0.016f;
    g_lastPresent = now;
    g_status.gameFps = g_status.gameFps * 0.95f + (dt > 0 ? 1.0f / dt : 0) * 0.05f;

    {
        // Reverse-engineering aid: where is Present called from (menu / in game)?
        static int menuStacks = 0, gameStacks = 0;
        if (Cfg().debugGpuTrace && g_status.luaRequests == 0 && menuStacks < 1) { ++menuStacks; LogStack("present-menu"); }
        if (Cfg().debugGpuTrace && g_status.luaRequests > 0 && gameStacks < 3) { ++gameStacks; LogStack("present-game"); }
    }
    PumpXr();
    UpdateRenderResolution();

    if (g_traceGpuSubmits) {
        DXGI_SWAP_CHAIN_DESC1 scd;
        sc->GetDesc1(&scd);
        ID3D12Resource* bufs[8] = {};
        UINT nb = std::min<UINT>(scd.BufferCount, 8);
        for (UINT i = 0; i < nb; ++i)
            if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&bufs[i])))) bufs[i]->Release();
        GpuTraceSetWatched(bufs, (int)nb);
    }

    // Back buffer
    UINT bbIndex = sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb)))) return;
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    g_bbW = (int)bd.Width;
    g_bbH = (int)bd.Height;
    g_status.backbufferW = g_bbW;
    g_status.backbufferH = g_bbH;
    g_status.backbufferFormat = bd.Format;
    DXGI_FORMAT bbFmt = Render::TypelessToTyped(bd.Format);

    bool xrLive, shouldRender, stereo, eyeFrame;
    int eye;
    int engineEye = EngineConsumeFrameEye();   // synchronised stereo: 1 = frozen right-eye frame
    {
        // Time spent on each kind of frame (Present to Present).
        static int counts[3] = {}, frames = 0;
        static double ms[3] = {}, last = 0, worst = 0;
        static int late = 0, slowWork = 0;
        static double worstWork = 0;
        double t = Now();
        if (last > 0) {
            double f = (t - last) * 1000.0;
            ms[engineEye + 1] += f;
            worst = std::max(worst, f);
            if (f > 20.0) ++late;   // a hitch: one engine frame longer than 20 ms
            // The game's own work in this frame (without waiting for the headset).
            double work = f - g_lastWaitMs;
            worstWork = std::max(worstWork, work);
            if (work > 14.0) ++slowWork;
            // Where the time of a slow frame went (a few per stats line).
            static int detailed = 0;
            if (frames % 600 == 0) detailed = 0;
            if (work > 18.0 && detailed < 6) {
                ++detailed;
                const double known = ProfKnownBase();   // (the sections perfTrace adds are parts of the rest)
                Log("Slow frame %.1f ms (eye %d): update+scripts %.1f, render %.1f, VR submit %.1f, present %.1f, "
                    "headset wait %.1f (xrWaitFrame %.1f), physics/other %.1f%s%s", f, engineEye, g_prof[P_Update],
                    g_prof[P_Render], g_prof[P_OurPresent], g_prof[P_RealPresent], g_prof[P_PostPresent], g_lastWaitMs,
                    f - known, g_perfTrace ? Perf::SlowFrameText(f - known).c_str() : "",
                    engineEye == 0 ? Perf::SlowFrameMods().c_str() : "");
                double now = ProfNow();
                SamplerReport(now - f, now);
            }
            if (g_perfTrace) Perf::FrameEnd(engineEye, f);
        }
        for (double& v : g_prof) v = 0;
        g_lastWaitMs = 0;
        last = t;
        counts[engineEye + 1]++;
        if (++frames % 600 == 0) {
            // ([Debug] perfTrace: per-eye sections, waits, engine profiler, CPU per thread group appended)
            // ([Performance] frameBMode, frameb.h: the right-eye frame B's work; "" when off)
            const std::string frameB = FrameB::StatsText();
            // ([Performance] lazyBeginFrame, perf.h: frames A whose beginRender waited for the first renderer use)
            const std::string lazy = Perf::LazyStatsText();
            // ([Performance] vdStats: Virtual Desktop's holds, the PC's send rate, the codec; "" when off)
            const std::string stream = Perf::StreamStatsText(HiddenFillPct());
            Log("Stereo frames: left=%d right=%d other=%d (game %.0f fps, headset %.0f fps) avg ms left=%.1f right=%.1f "
                "other=%.1f, xrWaitFrame %.1f ms x%d, worst frame %.1f ms, %d frames > 20 ms; slow game frames %d (worst %.1f ms), long waits %d (worst %.1f ms), physics catch-ups dropped %llu, "
                "headset refreshes missed %d (%d while the runtime held the frame)%s%s%s%s",
                counts[1], counts[2], counts[0], g_status.gameFps, g_status.vrFps, counts[1] ? ms[1] / counts[1] : 0,
                counts[2] ? ms[2] / counts[2] : 0, counts[0] ? ms[0] / counts[0] : 0, g_waits ? g_waitMs / g_waits : 0,
                g_waits, worst, late, slowWork, worstWork, g_waitLong, g_waitMax, (unsigned long long)EngineCatchUpDrops(),
                g_displayMissed, g_displayMissedRt, stream.c_str(), frameB.c_str(), g_perfTrace ? Perf::StatsText().c_str() : "",
                lazy.c_str());
            FrameB::LogDetails();
            if (g_holdsSmoothed) {
                Log("VR: %d runtime holds since the last line - the game resumed where it stopped after each "
                    "(%.0f ms of game time not played instead of jumps ahead; holdSmooth)", g_holdsSmoothed, g_holdNotPlayedMs);
                g_holdsSmoothed = 0;
                g_holdNotPlayedMs = 0;
            }
            if (g_perfTrace) Perf::LogDetails();
            Perf::CheckOverlays();   // (an overlay can be injected after the first frame)
            worst = worstWork = g_waitMax = 0;
            late = slowWork = g_waitLong = g_displayMissed = g_displayMissedRt = 0;
            g_waitMs = 0;
            g_waits = 0;
            counts[0] = counts[1] = counts[2] = 0;
            ms[0] = ms[1] = ms[2] = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        xrLive = g_session && g_running && g_frameBegun;
        shouldRender = xrLive && g_frameState.shouldRender;
        eyeFrame = (g_requestedThisFrame && g_requestActive) || engineEye == 1;   // one eye rendered this frame
        stereo = shouldRender && eyeFrame;
        eye = engineEye == 1 ? 1 : g_requestEye;
        // Simulated headset: alternate the eyes without a real XR frame loop.
        if (!xrLive && Cfg().debugFakeHmd && eyeFrame) {
            g_leftDone = (eye == 0);
            g_status.stereoActive = true;
        }
        g_requestedThisFrame = false;
        g_requestActive = false;
    }
    if (xrLive || !Cfg().debugFakeHmd) g_status.stereoActive = stereo;
    // (pauseWide: a paused level is drawn for a 16:9 screen only while non-eye frames go to the headset's virtual
    // screen - read by the next frame's update)
    EngineSetScreenLive(xrLive && shouldRender);
    // ([Performance] frameLatency: the right-eye frame's CPU work while the GPU draws the left eye)
    Perf::FrameLatency(sc, g_session && g_running);
    // ([Performance] jobSpin: frame A's job phases are over - no spinning in the headset wait nor in frame B)
    Perf::SpinPresent();
    if (engineEye >= 0) g_status.nativeStereo = true;

    ID3D12GraphicsCommandList* cl = Render::Begin();
    Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (Cfg().debugSyncTest) SyncTestCapture(cl, bb, bbFmt, eyeFrame ? eye : -1);
    if ((Cfg().debugSyncTest || Cfg().debugFovTest) && g_dumpCount == -1 && g_status.luaRequests > 400) g_dumpCount = 0;
    DumpCapture(cl, bb, bbFmt, eyeFrame ? eye : -1);
    D3D12_GPU_DESCRIPTOR_HANDLE bbSrv = Render::TransientSrv(bb, bbFmt);

    BlitParams bp;
    bp.linearize = (bbFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 0.0f : 1.0f;
    bp.sharpen = Cfg().sharpen;
    bp.brightness = Cfg().brightness;
    bp.texel[0] = 1.0f / g_bbW;
    bp.texel[1] = 1.0f / g_bbH;

    bool wroteScreen = false;
    if (shouldRender) {
        EnsureSwapchains();
        if (stereo) {
            if (ID3D12Resource* img = Acquire(g_eyeSc[eye])) {
                BlitParams ep = bp;
                ep.vignette = Cfg().vignette ? g_luaVignette * Cfg().vignetteStrength : 0.0f;
                // fovCrop: this image covers the headset's whole field; the game drew the covered part
                // (fovCoverage) - placed where that part lies, black around (CropRect; the whole image at 100 %).
                const D3D12_RECT cr = CropRect(eye, g_eyeSc[eye].w, g_eyeSc[eye].h);
                if (cr.left > 0 || cr.top > 0 || cr.right < g_eyeSc[eye].w || cr.bottom < g_eyeSc[eye].h)
                    ClearOutside(cl, img, cr, g_eyeSc[eye].w, g_eyeSc[eye].h);
                Render::Blit(cl, bbSrv, Render::TransientRtv(img, g_colorFormat), g_colorFormat, (UINT)cr.left,
                             (UINT)cr.top, (UINT)(cr.right - cr.left), (UINT)(cr.bottom - cr.top), ep);
                // A mod's interactive window: its cursor (engine.cpp, ModUiPointer), over this eye's
                // image at the pixel the window reads as the mouse in this eye (seen on its canvas).
                // A menu on its floating panel: the cursor is blue where the pointing hand's grip
                // would move the panel, and while a ray is on it, small blue squares mark the
                // corners of the zone a grip moves it from (ModPanelHighlight).
                bool grabCursor = false, corners = false;
                ModPanelHighlight(grabCursor, corners);
                const LONG ew = g_eyeSc[eye].w, eh = g_eyeSc[eye].h;
                auto clip = [&](LONG a, LONG b, LONG c, LONG d) {
                    return D3D12_RECT{ std::max<LONG>(0, a), std::max<LONG>(0, b), std::min(ew, c), std::min(eh, d) };
                };
                const float dark[4] = { 0.05f, 0.05f, 0.05f, 1 }, orange[4] = { 1, 0.62f, 0.2f, 1 },
                            blue[4] = { 0.3f, 0.72f, 1, 1 };
                float xy[8];
                if (corners && EngineModPanelCorners(eye, xy)) {
                    const LONG r = std::max<LONG>(3, ew / 400);
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = Render::TransientRtv(img, g_colorFormat);
                    for (int k = 0; k < 4; ++k) {
                        const LONG x = cr.left + (LONG)(xy[2 * k] * (cr.right - cr.left)),
                                   y = cr.top + (LONG)(xy[2 * k + 1] * (cr.bottom - cr.top));
                        if (x < 0 || y < 0 || x >= ew || y >= eh) continue;
                        const D3D12_RECT outer = clip(x - r - 2, y - r - 2, x + r + 2, y + r + 2);
                        const D3D12_RECT inner = clip(x - r, y - r, x + r, y + r);
                        cl->ClearRenderTargetView(rtv, dark, 1, &outer);
                        cl->ClearRenderTargetView(rtv, blue, 1, &inner);
                    }
                }
                float cu, cv;
                if (EngineModUiCursor(eye, cu, cv)) {
                    const LONG x = cr.left + (LONG)(cu * (cr.right - cr.left)), y = cr.top + (LONG)(cv * (cr.bottom - cr.top)),
                               r = std::max<LONG>(4, ew / 250);
                    if (x >= 0 && y >= 0 && x < ew && y < eh) {
                        const D3D12_RECT outer = clip(x - r - 3, y - r - 3, x + r + 3, y + r + 3);
                        const D3D12_RECT inner = clip(x - r, y - r, x + r, y + r);
                        D3D12_CPU_DESCRIPTOR_HANDLE rtv = Render::TransientRtv(img, g_colorFormat);
                        cl->ClearRenderTargetView(rtv, dark, 1, &outer);
                        cl->ClearRenderTargetView(rtv, grabCursor ? blue : orange, 1, &inner);
                    }
                }
                // ([Performance] hiddenAreaFill: what the lenses never show, cleared before the release)
                FillHidden(cl, img, eye);
                auto& pv = g_projViews[eye];
                pv.pose = ToXr(g_renderPose[eye]);
                pv.fov = g_submitFov[eye];   // (fovCrop: the headset's whole field, see CropRect)
                pv.subImage.swapchain = g_eyeSc[eye].handle;
                pv.subImage.imageRect = { { 0, 0 }, { g_eyeSc[eye].w, g_eyeSc[eye].h } };
                pv.subImage.imageArrayIndex = 0;
            }
        } else if (ID3D12Resource* img = Acquire(g_screenSc)) {
            BlitParams sp = bp;
            sp.sharpen = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = Render::TransientRtv(img, g_colorFormat);
            // pauseWide: a paused level drawn for a 16:9 screen (its 2D canvas and 3D view stretched over the window:
            // engine.cpp, WideState) goes whole into the image's top 16:9 band, shown on a 16:9 quad - the desktop
            // game's proportions, square pixels, the mouse mapping unchanged (a quad point is the same fraction of
            // the window). The rest of the image (outside the layer's imageRect) black.
            const bool wide = EngineWideFrame();
            g_screenUseW = g_screenSc.w;
            g_screenUseH = wide ? std::min(g_screenSc.h, std::max(1, (g_screenSc.w * 9 + 8) / 16)) : g_screenSc.h;
            if (g_screenUseH < g_screenSc.h) {
                const float black[4] = { 0, 0, 0, 1 };
                const D3D12_RECT rest = { 0, g_screenUseH, g_screenSc.w, g_screenSc.h };
                cl->ClearRenderTargetView(rtv, black, 1, &rest);
            }
            {
                static int lastW = 0, lastH = 0, logs = 0;
                if ((g_screenUseW != lastW || g_screenUseH != lastH) && logs < 40) {
                    ++logs;
                    Log("VR: virtual screen %s: %dx%d of its %dx%d image, %.2f x %.2f m",
                        wide ? "16:9 (paused level, pauseWide)" : "= the whole game image", g_screenUseW, g_screenUseH,
                        g_screenSc.w, g_screenSc.h, Cfg().screenWidth, Cfg().screenWidth * g_screenUseH / (float)g_screenUseW);
                }
                lastW = g_screenUseW;
                lastH = g_screenUseH;
            }
            Render::Blit(cl, bbSrv, rtv, g_colorFormat, 0, 0, g_screenUseW, g_screenUseH, sp);
            // The controller's pointer on the virtual screen (the game draws no cursor here).
            if (g_screenPointerU >= 0) {
                int x = (int)(g_screenPointerU * g_screenUseW), y = (int)(g_screenPointerV * g_screenUseH);
                int r = std::max(4, g_screenUseW / 160);
                const float dark[4] = { 0.05f, 0.05f, 0.05f, 1 }, light[4] = { 1, 0.62f, 0.2f, 1 };
                D3D12_RECT outer = { x - r - 2, y - r - 2, x + r + 2, y + r + 2 }, inner = { x - r, y - r, x + r, y + r };
                cl->ClearRenderTargetView(rtv, dark, 1, &outer);
                cl->ClearRenderTargetView(rtv, light, 1, &inner);
            }
            // The other hand's ray on it (offHandPointer): a smaller grey mark - the menus have no lasers; that
            // hand's trigger takes the pointer and clicks there (Input::UpdatePointerHand). Not drawn where it
            // would cover the pointer's orange mark above (both rays on one spot: the orange one shows it).
            if (g_screenOtherU >= 0) {
                const LONG W = g_screenUseW, H = g_screenUseH;   // (the image's part in use: pauseWide)
                const LONG x = (LONG)(g_screenOtherU * W), y = (LONG)(g_screenOtherV * H);
                const LONG r = std::max<LONG>(3, W / 240);
                // (the orange mark: half size max(4, W / 160) and its 2-pixel frame, at the pointer's u, v)
                const LONG reach = r + 2 + std::max<LONG>(4, W / 160) + 2;
                const LONG dx = x - (LONG)(g_screenPointerU * W), dy = y - (LONG)(g_screenPointerV * H);
                const bool overPointer = g_screenPointerU >= 0 && dx > -reach && dx < reach && dy > -reach && dy < reach;
                if (!overPointer) {
                    const float dark[4] = { 0.05f, 0.05f, 0.05f, 1 }, grey[4] = { 0.8f, 0.8f, 0.8f, 1 };
                    auto box = [&](LONG m) {
                        return D3D12_RECT{ std::max<LONG>(0, x - m), std::max<LONG>(0, y - m), std::min(W, x + m), std::min(H, y + m) };
                    };
                    const D3D12_RECT outer = box(r + 2), inner = box(r);
                    cl->ClearRenderTargetView(rtv, dark, 1, &outer);
                    cl->ClearRenderTargetView(rtv, grey, 1, &inner);
                }
            }
            wroteScreen = true;
        }
    }

    // Our floating panels: the VR inventory worn by the body (straight in front of it when it
    // appears), a panel moved by a grip following that hand's ray.
    UpdatePanels(dt, Inventory::Visible() && Inventory::ConsumePlaceRequest());

    // Settings UI
    D3D12_GPU_DESCRIPTOR_HANDLE uiSrv = {};
    ID3D12Resource* uiTex = Ui::Draw(cl, &uiSrv);
    bool uiInVr = false;
    if (uiTex && shouldRender) {
        if (ID3D12Resource* img = Acquire(g_uiSc)) {
            BlitParams up;
            up.linearize = 1;
            up.opaque = 0;
            Render::Blit(cl, uiSrv, Render::TransientRtv(img, g_colorFormat), g_colorFormat, 0, 0, g_uiSc.w, g_uiSc.h, up);
            uiInVr = true;
        }
    }
    if (uiTex && Cfg().showDesktopUi) {
        Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        BlitParams up;
        up.opaque = 0;
        UINT w = std::min<UINT>(Ui::kWidth, g_bbW), h = std::min<UINT>(Ui::kHeight, g_bbH);
        Render::Blit(cl, uiSrv, Render::TransientRtv(bb, bbFmt), bbFmt, 0, 0, w, h, up, Blend::Premultiplied);
        Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        Render::Barrier(cl, bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
    }
    Render::Submit();
    bb->Release();
    DumpWrite();

    for (auto& s : g_eyeSc) Release(s);
    Release(g_screenSc);
    Release(g_uiSc);

    // Submit to the headset when a frame is complete.
    if (xrLive) {
        bool leftDone;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            leftDone = g_leftDone;
        }
        XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerQuad screen = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrCompositionLayerQuad ui = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrCompositionLayerQuad banner = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        const XrCompositionLayerBaseHeader* layers[4];
        uint32_t layerCount = 0;
        bool endNow = true;

        if (!shouldRender) {
            // nothing to show
        } else if (stereo && eye == 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_leftDone = true;
            endNow = false;
        } else if (stereo && eye == 1 && leftDone) {
            proj.space = g_appSpace;
            proj.viewCount = 2;
            proj.views = g_projViews;
            layers[layerCount++] = (XrCompositionLayerBaseHeader*)&proj;
        } else if (g_screenSc.handle) {
            screen.space = g_appSpace;
            screen.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            screen.subImage.swapchain = g_screenSc.handle;
            int useW = 0, useH = 0;   // (the part of the image in use: 16:9 in a paused level, ScreenUse)
            ScreenUse(useW, useH);
            screen.subImage.imageRect = { { 0, 0 }, { useW, useH } };
            screen.pose = ToXr(ScreenPose());
            screen.size = { Cfg().screenWidth, ScreenHeightM() };
            layers[layerCount++] = (XrCompositionLayerBaseHeader*)&screen;
            (void)wroteScreen;
        }

        if (endNow && shouldRender && uiTex && uiInVr && g_uiSc.handle) {
            ui.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            ui.space = g_appSpace;
            ui.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            ui.subImage.swapchain = g_uiSc.handle;
            ui.subImage.imageRect = { { 0, 0 }, { g_uiSc.w, g_uiSc.h } };
            ui.pose = ToXr(g_uiPose);
            float w = 1.0f * Cfg().uiScale;
            ui.size = { w, w * Ui::kHeight / (float)Ui::kWidth };
            // Warning banner (panel closed): follows the head, 1.5 m away and
            // a little below the line of sight; 0.8 m wide = about 30 degrees.
            auto toBanner = [](XrCompositionLayerQuad& q) {
                q.space = g_viewSpace;
                q.subImage.imageRect = { { 0, Ui::BannerTop() }, { Ui::kBannerW, Ui::kBannerH } };
                q.pose = { { 0, 0, 0, 1 }, { 0, -0.28f, -1.5f } };
                q.size = { 0.8f, 0.8f * Ui::kBannerH / (float)Ui::kBannerW };
            };
            if (Ui::InventoryShown()) {
                // VR inventory (tools and weapons): the top-left corner of the texture.
                int pw = std::min(Inventory::PanelW(), Ui::kWidth), ph = std::min(Inventory::PanelH(), Ui::kHeight);
                float mpp = Inventory::kMetersPerPixel * Cfg().uiScale;
                ui.subImage.imageRect = { { 0, 0 }, { pw, ph } };
                ui.pose = ToXr(g_invPose);
                ui.size = { pw * mpp, ph * mpp };
                if (Ui::BannerShown() && g_viewSpace) {   // (both: the banner is a second quad)
                    banner = ui;
                    toBanner(banner);
                }
            } else if (Ui::BannerShown() && g_viewSpace) {
                toBanner(ui);
            }
            layers[layerCount++] = (XrCompositionLayerBaseHeader*)&ui;
            if (banner.subImage.swapchain) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&banner;
        }

        if (endNow) {
            EndXrFrame(layers, layerCount);
            double t = Now();
            float xdt = g_lastXrFrame > 0 ? float(t - g_lastXrFrame) : 0.011f;
            g_lastXrFrame = t;
            g_status.vrFps = g_status.vrFps * 0.95f + (xdt > 0 ? 1.0f / xdt : 0) * 0.05f;
        }
        // The headset paces the game, not the monitor. Without "allow tearing"
        // the desktop compositor still holds the game to the monitor's refresh
        // rate (measured: ~12 ms per frame with the CPU and GPU two-thirds idle).
        sync = 0;
        flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        DXGI_SWAP_CHAIN_DESC1 scd;
        BOOL exclusive = FALSE;
        sc->GetFullscreenState(&exclusive, nullptr);
        if (SUCCEEDED(sc->GetDesc1(&scd)) && (scd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) && !exclusive)
            flags |= DXGI_PRESENT_ALLOW_TEARING;
    }

    // Pointers & input
    HandState L, R;
    bool haveHands = VrGetHands(L, R);
    if (!haveHands) { L = {}; R = {}; }
    if (xrLive && haveHands) {
        HandState rawR, rawL;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            rawL = g_handsRaw[0];
            rawR = g_handsRaw[1];
        }
        // Either hand points at our panels (settings, VR inventory, virtual screen):
        // Input::PointerHand() is the hand whose trigger was pulled last with its ray on one (never
        // the other hand only because its ray is there: see input.h); the code below follows that
        // hand. (The trigger of a hand
        // moving a panel with its grip - a whole-fist squeeze - clicks nothing: VrPanelMasksTrigger.)
        const float trig[2] = { g_trigMasked[0] ? 0.0f : L.trigger, g_trigMasked[1] ? 0.0f : R.trigger };
        {
            const HandState* raw[2] = { &rawL, &rawR };
            bool on[2] = {};
            const float uw = 1.0f * Cfg().uiScale, uh = uw * Ui::kHeight / (float)Ui::kWidth;
            const float imp = Inventory::kMetersPerPixel * Cfg().uiScale;
            const float sw = Cfg().screenWidth, sh = ScreenHeightM();   // (16:9 in a paused level: pauseWide)
            for (int i = 0; i < 2; ++i) {
                if (!raw[i]->active) continue;
                float hu = 0, hv = 0;
                bool inside = false;
                if (Ui::Visible() && RayQuad(raw[i]->aim, g_uiPose, uw, uh, hu, hv)) on[i] = true;
                if (Inventory::Visible() && g_invFocus &&
                    RayPlane(raw[i]->aim, g_invPose, Inventory::PanelW() * imp, Inventory::PanelH() * imp, hu, hv, inside) &&
                    inside)
                    on[i] = true;
                if (!stereo && g_screenSc.handle && RayQuad(raw[i]->aim, ScreenPose(), sw, sh, hu, hv)) on[i] = true;
            }
            Input::UpdatePointerHand(on[0], on[1], trig[0], trig[1]);
        }
        const bool leftPointer = Input::PointerHand() == 0;
        const HandState& pointer = leftPointer ? rawL : rawR;
        const float pointerTrig = trig[leftPointer ? 0 : 1];
        float u, v;
        if (Ui::Visible()) {
            float w = 1.0f * Cfg().uiScale, h = w * Ui::kHeight / (float)Ui::kWidth;
            if (g_trigMasked[leftPointer ? 0 : 1]) {
                // (the hand moving the panel points at nothing on it, button up: a trigger pressed
                // before its grip must not click the button under the ray when it is let go - ImGui
                // clicks on release; a key of the keys panel: Ui::SetHeld. Still the VR pointer, off
                // the panel: the PC mouse does not take the cursor over meanwhile)
                Ui::SetVrPointer(true, -10000.0f, -10000.0f, false);
            } else {
                bool hit = pointer.active && RayQuad(pointer.aim, g_uiPose, w, h, u, v);
                Ui::SetVrPointer(hit, u * Ui::kWidth, v * Ui::kHeight, pointerTrig > 0.6f);
            }
        } else {
            Ui::SetVrPointer(false, 0, 0, false);
        }
        {
            // VR inventory: the pointer hand's ray on the panel's plane.
            bool hit = false, inside = false;
            float iu = 0, iv = 0;
            float pw = (float)Inventory::PanelW(), ph = (float)Inventory::PanelH();
            float mpp = Inventory::kMetersPerPixel * Cfg().uiScale;
            if (Inventory::Visible() && pointer.active)
                hit = RayPlane(pointer.aim, g_invPose, pw * mpp, ph * mpp, iu, iv, inside);
            Inventory::SetPointer(hit, inside, iu * pw, iv * ph, pointerTrig, g_invFocus);
        }
        if (!stereo && g_screenSc.handle) {
            float sw = Cfg().screenWidth, sh = ScreenHeightM();
            if (pointer.active && RayQuad(pointer.aim, ScreenPose(), sw, sh, u, v)) {
                Input::SetScreenPointer(u, v);
                g_screenPointerU = u;
                g_screenPointerV = v;
            } else {
                Input::SetScreenPointer(-1, -1);
                g_screenPointerU = g_screenPointerV = -1;
            }
            // The other hand's ray on the screen (offHandPointer): only its grey mark (drawn above) - the
            // game's cursor follows the pointer hand alone, also when that hand's ray is beside the screen.
            const HandState& otherHand = leftPointer ? rawR : rawL;
            float ou = 0, ov = 0;
            if (Cfg().offHandPointer && otherHand.active && RayQuad(otherHand.aim, ScreenPose(), sw, sh, ou, ov)) {
                g_screenOtherU = ou;
                g_screenOtherV = ov;
            } else {
                g_screenOtherU = g_screenOtherV = -1;
            }
        }
    }
    if (!(xrLive && haveHands)) {
        Inventory::SetPointer(false, false, 0, 0, 0.0f, false);
        g_screenOtherU = g_screenOtherV = -1;   // (no hands: no mark of the other hand)
    }
    bool fakeLive = !xrLive && Cfg().debugFakeHmd && eyeFrame;
    if (fakeLive) FakeHands(L, R);
    // The controllers only drive the game while the session has the input focus
    // (not while the headset's own system menu is open).
    Input::Mode mode = xrLive ? (!g_status.focused ? Input::Mode::Off
                                 : stereo      ? (g_mapView ? Input::Mode::Map : Input::Mode::Gameplay)
                                               : Input::Mode::Screen)
                              : (fakeLive ? Input::Mode::Gameplay : Input::Mode::Off);
    Input::Update(mode, L, R, Ui::Visible(), dt);

    // Haptics requested by Lua
    if (xrLive && g_actionSet) {
        for (int i = 0; i < 2; ++i) {
            if (g_hapticReq[i] > 0 && Cfg().haptics) VrHaptic(i, g_hapticReq[i], 0.03f);
            g_hapticReq[i] = 0;
        }
    }
}

void OnGamePostPresent()
{
    Input::FlushDeferred();
    bool need;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        need = g_session && g_running && !g_frameBegun;
    }
    if (need) BeginXrFrame();
}

// ---------------------------------------------------------------------------
// Lua bridge
// ---------------------------------------------------------------------------
namespace {
void AppendPose(std::string& s, const Pose& p)
{
    char b[160];
    snprintf(b, sizeof(b), " %.4f %.4f %.4f %.5f %.5f %.5f %.5f", p.p.x, p.p.y, p.p.z, p.q.x, p.q.y, p.q.z, p.q.w);
    s += b;
}
} // namespace

namespace {
// Symmetric frustum (what the game can render) covering the headset's
// asymmetric field of view for one eye (cov: the share of it, FovCoverage; 1 = all).
XrFovf RenderFovFor(const XrFovf& f, float cov)
{
    float needH = std::max(std::tan(-f.angleLeft), std::tan(f.angleRight)) * cov;
    float needV = std::max(std::tan(f.angleUp), std::tan(-f.angleDown)) * cov;
    float aspect = g_bbW / (float)g_bbH;
    float tanH = std::max(needH, needV * aspect);
    tanH = std::min(tanH, std::tan(84.0f * kDeg));
    float tanV = tanH / aspect;
    return { -std::atan(tanH), std::atan(tanH), std::atan(tanV), -std::atan(tanV) };
}

// The headset's own (asymmetric) field of view for one eye, scaled by cov (the
// coverage setting, FovCoverage; 1 = all), widened only as needed to fill the render target with square
// pixels (the target's aspect is chosen at game start from this same field of view).
XrFovf OffCenterFovFor(const XrFovf& f, float cov)
{
    float lim = std::tan(84.0f * kDeg);
    float tl = std::min(std::tan(-f.angleLeft) * cov, lim), tr = std::min(std::tan(f.angleRight) * cov, lim);
    float tu = std::min(std::tan(f.angleUp) * cov, lim), td = std::min(std::tan(-f.angleDown) * cov, lim);
    float aspect = g_bbW / (float)g_bbH;
    float h = tl + tr, v = tu + td;
    if (h < v * aspect) {
        float add = (v * aspect - h) * 0.5f;
        tl += add;
        tr += add;
    } else {
        float add = (h / aspect - v) * 0.5f;
        tu += add;
        td += add;
    }
    return { -std::atan(tl), std::atan(tr), std::atan(tu), -std::atan(td) };
}

// The off hand's laser (main.lua, tdvrOffLaserTick) stops where its ray meets one of our panels,
// the ones its trigger can drive: the settings panel, the VR inventory while the player looks
// toward it (the same tests as Input::UpdatePointerHand), a mod's menu on its floating panel
// (EngineGetModPanel: its content or frame band), or another mod's interactive window while
// this hand's ray is on it (EngineModUiOnWindow, from the left eye of the last stereo pair: the
// plane modUiDistance in front of the eyes, as engine.cpp's ModUiRayPoint).
// Distance along the ray from the controller's aim, metres; -1 = none. hand: 0 left, 1 right;
// aimRc and the eyes are in recentred room space (the panels' poses are in app space, like
// g_handsRaw; distances are the same in both). Called with g_mutex held.
float OffLaserStop(int hand, const Pose& aimRc, const Pose& eyeL, const Pose& eyeR)
{
    const HandState& raw = g_handsRaw[hand];
    if (!raw.active || !Cfg().offLaser) return -1;
    float best = -1, u = 0, v = 0;
    // (the ray's direction is a unit vector and the quad's pose is rigid: t is in metres)
    auto plane = [&](const Pose& quad) {
        const Pose inv = Inverse(quad);
        const Vec3 o = Rotate(inv.q, raw.aim.p) + inv.p, d = Rotate(inv.q, Rotate(raw.aim.q, { 0, 0, -1 }));
        if (std::abs(d.z) < 1e-5f) return;
        const float t = -o.z / d.z;
        if (t > 0 && (best < 0 || t < best)) best = t;
    };
    const float uw = 1.0f * Cfg().uiScale, uh = uw * Ui::kHeight / (float)Ui::kWidth;
    if (Ui::Visible() && RayQuad(raw.aim, g_uiPose, uw, uh, u, v)) plane(g_uiPose);
    const float mpp = Inventory::kMetersPerPixel * Cfg().uiScale;
    bool inside = false;
    if (Inventory::Visible() && g_invFocus &&
        RayPlane(raw.aim, g_invPose, Inventory::PanelW() * mpp, Inventory::PanelH() * mpp, u, v, inside) && inside)
        plane(g_invPose);
    EngineModPanel mp;
    if (EngineGetModPanel(mp)) {
        // (a mod's menu on its floating panel: on its content or frame band - not under our panels,
        // seen over it whatever the distances)
        float t = 0;
        if (best < 0 && ModPanelZone(aimRc, mp, t, u, v)) best = t;
    } else if (best < 0 && EngineModUiOnWindow(hand)) {
        const Vec3 mid = (eyeL.p + eyeR.p) * 0.5f, f = Rotate(eyeL.q, { 0, 0, -1 }), d = Rotate(aimRc.q, { 0, 0, -1 });
        const float den = Dot(d, f);
        if (den >= 0.05f) {
            const float t = (std::clamp(Cfg().modUiDistance, 0.3f, 5.0f) - Dot(aimRc.p - mid, f)) / den;
            if (t > 0) best = t;
        }
    }
    return best;
}

std::string BuildResponse(int seq, int eye, const XrFovf& rf, const Pose& eyeRc, const Pose& headRc,
                          const HandState& hl, const HandState& hr, bool frozen = false, float offLaserStop = -1.0f)
{
    float fovH = (rf.angleRight - rf.angleLeft) / kDeg, fovV = (rf.angleUp - rf.angleDown) / kDeg;
    // SetCameraFov does not take the horizontal field of view of the image: the
    // engine (camera projection setup, Teardown 2.0.1 ~RVA 0x5ebcc0) computes
    //   tan(vertical/2) = tan(fov/2) * 9/16,   tan(horizontal/2) = tan(vertical/2) * width/height
    // i.e. "fov" is a horizontal field of view for a 16:9 image. Matching only
    // at 16:9 - with a square-ish VR image the game rendered a ~46% narrower
    // view than the headset expected (warped "fish-eye" view, no stereo fusion).
    float engineFov = 2.0f * std::atan(std::tan(rf.angleUp) * 16.0f / 9.0f) / kDeg;
    g_status.renderFovH = fovH;
    g_status.renderFovV = fovV;
    std::string s;
    s.reserve(1024);
    char b[512];
    snprintf(b, sizeof(b), "TDVR2 %d 1 %d %.3f %.3f %.4f %d", seq, eye, engineFov, fovV, g_refHeadY, g_recenterCount);
    s += b;
    AppendPose(s, eyeRc);
    AppendPose(s, headRc);
    AppendPose(s, hl.aim);
    AppendPose(s, hr.aim);
    AppendPose(s, hl.grip);
    AppendPose(s, hr.grip);
    int buttons = (hr.primary ? 1 : 0) | (hr.secondary ? 2 : 0) | (hl.primary ? 4 : 0) | (hl.secondary ? 8 : 0) |
                  (hl.stickClick ? 16 : 0) | (hr.stickClick ? 32 : 0) | (hl.menu ? 64 : 0) |
                  (hl.active ? 128 : 0) | (hr.active ? 256 : 0);
    snprintf(b, sizeof(b), " %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %d", hl.stickX, hl.stickY, hr.stickX, hr.stickY,
             hl.trigger, hr.trigger, hl.squeeze, hr.squeeze, buttons);
    s += b;
    const Settings& c = Cfg();
    snprintf(b, sizeof(b), " %d %.2f %.2f %d %.3f %d %.3f %d %.3f %d %d %d %d %.3f %.3f %.3f %.2f %d %.3f %d %d %d %d %d %d %d %d %d %d",
             c.turnMode, c.snapAngle, c.smoothTurnSpeed, c.moveDirection, c.moveSpeed, c.roomscale ? 1 : 0,
             c.heightOffset, c.physicalHeight ? 1 : 0, c.worldScale, c.leftHanded ? 1 : 0, c.aimMode, c.hideHud ? 1 : 0,
             c.laserPointer ? 1 : 0, c.toolOffsetX, c.toolOffsetY, c.toolOffsetZ, c.toolPitch, c.vignette ? 1 : 0,
             c.vignetteStrength, Ui::Visible() ? 1 : 0, (c.debugLua ? 1 : 0) | (c.debugSyncTest ? 2 : 0) | (c.debugFovTest ? 4 : 0) | (c.debugNoAim ? 8 : 0) |
                 (EnginePlayerYawAvailable() ? 16 : 0) |
                 (g_status.runtime.find("Mock") != std::string::npos ? 32 : 0) |  // casque simulé (tests)
                 (c.fastLua ? 64 : 0),                                             // [Performance] fastLua (main.lua)
             frozen ? 1 : 0, c.removeFog ? 1 : 0, c.swingGesture ? 1 : 0, c.removeBloom ? 1 : 0,
             c.toolCollision ? 1 : 0, c.toolBlock ? 1 : 0, c.removeDof ? 1 : 0,
             c.toolPhysics ? 1 : 0);
    s += b;
    // The tool in the hand (EngineGetToolInfo): kind, desktop pose of a mod's tool,
    // the right hand the mod placed on it.
    EngineToolInfo ti = EngineGetToolInfo();
    snprintf(b, sizeof(b), " %d", ti.kind);
    s += b;
    AppendPose(s, ti.flat);
    snprintf(b, sizeof(b), " %d %.4f %.4f %.4f %.4f", ti.hasHand ? 1 : 0, ti.hand.x, ti.hand.y, ti.hand.z, ti.scale);
    s += b;
    // " PIV x y z": the muzzle of a mod's tool, about which it is scaled (tool body,
    // unscaled), found by its tag after the numbered fields: the Lua mod rebuilds what it
    // measured in the tool body when it changes.
    snprintf(b, sizeof(b), " PIV %.3f %.3f %.3f", ti.pivot.x, ti.pivot.y, ti.pivot.z);
    s += b;
    // " ARM <arm> <physics> <grab> <grip> <keep> <stick>": draw the hand that holds no tool (main.lua,
    // tdvrOffArmTick); make it a physical body (Bonelabs-like, tdvrOffHandTick); let its
    // grip take objects; that grip's current press (Input::OffGripPress: 0 up, 1 crouch,
    // 2 grab, decided when it went down); grabKeepDistance (an object taken at a distance stays
    // there) and how fast the dominant stick moves it (Input::OffGrabStick, m/s, + = farther).
    // An older Lua mod reads only the first fields.
    snprintf(b, sizeof(b), " ARM %d %d %d %d %d %.3f", c.offArm ? 1 : 0, c.offArm && c.offHandPhysics ? 1 : 0,
             c.offArm && c.offHandPhysics && c.offGrab ? 1 : 0, Input::OffGripPress(), c.grabKeepDistance ? 1 : 0,
             Input::OffGrabStick());
    s += b;
    // " LAS <laser> <stop>": draw a laser from that hand too (main.lua, tdvrOffLaserTick: with
    // the physical hand's grab, its grip pulls the object it points at); where its ray meets one
    // of our panels (OffLaserStop: metres from the controller's aim, -1 = none).
    snprintf(b, sizeof(b), " LAS %d %.3f", c.offLaser ? 1 : 0, offLaserStop);
    s += b;
    // " DUA <allowed> <shots> <gun> <ready>": dual wield (engine.cpp, DualEye; main.lua, tdvrDualTick):
    // the held tool is two guns to split (allowed); shots given to a gun so far and the gun of the last
    // one (1 = dominant hand, 0 = the other: the Lua mod vibrates that hand and kicks the copy); ready:
    // + 1 the dominant gun's data is fresh, + 2 the other's (diagnostics).
    int dAllowed = 0, dShots = 0, dGun = -1, dReady = 0;
    EngineDualState(dAllowed, dShots, dGun, dReady);
    snprintf(b, sizeof(b), " DUA %d %d %d %d", dAllowed, dShots % 1000, dGun, dReady);
    s += b;
    return s;
}

// Debug: a simulated headset so the whole Lua <-> DLL chain can be tested on
// the desktop without wearing the Quest.
std::string FakeHmdResponse(int seq)
{
    int eye;
    bool frozen = false;
    if (EngineStereoAvailable() && Cfg().stereoMode == 0) {
        // Synchronised stereo: left eye on normal frames, right eye on frozen ones.
        frozen = EngineInFrozenFrame();
        eye = frozen ? 1 : 0;
        if (!frozen) {
            EngineRequestStereo();
            EngineSetEyeOffset({ {}, { 0.064f * Cfg().ipdScale * Cfg().worldScale, 0, 0 } });
            EngineSetLeftEyeRotation(AxisAngle({ 0, 1, 0 }, (float)std::sin(Now() * 0.4) * 25.0f * kDeg));
        }
        g_requestEye = eye;
        g_requestedThisFrame = true;
    } else if (g_requestedThisFrame) {
        eye = g_requestEye;
    } else {
        eye = g_leftDone ? 1 : 0;
        g_requestEye = eye;
        g_requestedThisFrame = true;
    }
    g_requestActive = true;
    double t = Now();
    float yaw = (float)std::sin(t * 0.4) * 25.0f * kDeg;
    // The head also walks slowly around the room (tests room-scale).
    Pose head{ AxisAngle({ 0, 1, 0 }, yaw), { 0.3f * (float)std::sin(t * 0.3), 1.65f, 0.2f * (float)std::sin(t * 0.21) } };
    Pose eyeP = head;
    eyeP.p = head.p + Rotate(head.q, { (eye == 0 ? -0.032f : 0.032f) * Cfg().ipdScale, 0, 0 });
    HandState hl, hr;
    FakeHands(hl, hr);
    Input::MaskMouseStick(hl, hr);
    XrFovf quest3 = { -0.94f, 0.77f, 0.84f, -0.87f };
    if (eye == 1) quest3 = { -0.77f, 0.94f, 0.84f, -0.87f };
    XrFovf rf = RenderFovFor(quest3, FovCoverage());
    g_refHeadY = 1.65f;
    return BuildResponse(seq, eye, rf, eyeP, head, hl, hr, frozen);
}
} // namespace

std::string VrHandleLuaRequest(const LuaRequest& req)
{
    if (Cfg().debugGpuTrace) {
        if (g_status.luaRequests < 3) LogStack("lua-tick");
        if (g_status.luaRequests == 30) g_traceGpuSubmits = true;
        if (g_status.luaRequests == 300) GpuTraceArm();
    }
    if (Cfg().debugWatchCamera) {
        if (g_status.luaRequests == 120) {
            auto base = (uintptr_t)GetModuleHandleW(nullptr);
            uintptr_t game = *(uintptr_t*)(base + 0x1c6e270);
            uintptr_t cam = game ? *(uintptr_t*)(game + 0x60) : 0;
            if (cam) WatchMemory((void*)(cam + 0x1d0), 4);
        }
        if (g_status.luaRequests == 480) StopWatch();
    }
    g_status.luaRequests++;
    g_lastLuaRequest = Now();
    if (Cfg().debugLua) {
        static int lastSeq = 0, gaps = 0;
        if (lastSeq && req.seq != lastSeq + 1 && gaps < 40) {
            ++gaps;
            Log("Bridge diag: Lua seq jumped %d -> %d", lastSeq, req.seq);
            if (gaps <= 3) BridgeDumpTrace();
        }
        lastSeq = req.seq;
    }
    if (req.hapticL > 0) g_hapticReq[0] = std::max(g_hapticReq[0], req.hapticL);
    if (req.hapticR > 0) g_hapticReq[1] = std::max(g_hapticReq[1], req.hapticR);
    g_luaVignette = std::clamp(req.vignette, 0.0f, 1.0f);

    std::lock_guard<std::mutex> lock(g_mutex);
    bool active = g_session && g_running && g_frameBegun && g_frameState.shouldRender && g_viewsValid &&
                  !Cfg().forceScreen;
    // The level map (Tab / X): a 3D scale model in synchronised stereo (see
    // engine.cpp, MapView); otherwise shown on the virtual screen like a menu.
    bool map3d = EngineMapAvailable() && Cfg().stereoMode == 0;
    g_mapView = active && (req.flags & 4) && map3d;
    Input::SetVehicle(active && (req.flags & 1));
    Input::SetPlayerGrabbing(active && (req.flags & kLuaFlagGrabbing));
    Input::SetOffFarHold(active && (req.flags & kLuaFlagOffFarHold));
    Inventory::SetGameFlags(active ? req.flags : 0);
    EngineSetCameraNearPlayer((req.flags & kLuaFlagCamNearPlayer) != 0);   // see main.lua
    if (active && (req.flags & 4) && !map3d) {
        EngineSetVrActive(false);
        char head[64];
        snprintf(head, sizeof(head), "TDVR2 %d 2", req.seq);   // 2 = VR paused (map)
        return head;
    }
    EngineSetVrActive(active || (Cfg().debugFakeHmd && !g_running));
    // On foot, the Lua mod turns the player toward the controller's laser (main.lua,
    // updateAim): other scripts' GetCameraTransform() then follows that aim (engine.cpp).
    EngineSetHandAim(active && !(req.flags & kLuaFlagVehicle) && !(req.flags & kLuaFlagMap));
    if (!active) {
        if (Cfg().debugFakeHmd && !g_running) return FakeHmdResponse(req.seq) + Inventory::ResponseFields();
        char head[64];
        snprintf(head, sizeof(head), "TDVR2 %d 0", req.seq);
        return head;
    }

    // Everything handed to Lua is relative to the recentered origin.
    Pose inv = Inverse(g_recenter);
    Pose headRc = Mul(inv, g_headRaw);
    auto eyeRcOf = [&](int e) {
        Pose p = Mul(inv, ToPose(g_views[e].pose));
        p.p = headRc.p + (p.p - headRc.p) * Cfg().ipdScale;
        return p;
    };

    int eye;
    Pose eyeRc;
    bool frozen = false;
    if (EngineStereoAvailable() && Cfg().stereoMode == 0) {
        // Synchronised stereo: left eye on the normal frame, right eye on the
        // following frozen frame, both from the same headset pose prediction.
        frozen = EngineInFrozenFrame();
        eye = frozen ? 1 : 0;
        if (!frozen) {
            EngineRequestStereo();
            Pose rel = Mul(Inverse(eyeRcOf(0)), eyeRcOf(1));
            rel.p = rel.p * Cfg().worldScale;
            EngineSetEyeOffset(rel);
            EngineSetLeftEyeRotation(eyeRcOf(0).q);
            EngineSetLeftEyePosition(eyeRcOf(0).p);
            // (fovCrop: the game draws the covered part of the field, the eye images cover all of it - CropRect)
            const float cov = FovCoverage(), whole = Cfg().fovCrop ? 1.0f : cov;
            XrFovf f0 = RenderFovFor(g_views[0].fov, cov), f1 = RenderFovFor(g_views[1].fov, cov);
            const int pick = f0.angleRight >= f1.angleRight ? 0 : 1;   // identical in practice (mirror images)
            XrFovf f = pick ? f1 : f0;
            g_renderFov[0] = g_renderFov[1] = f;
            g_submitFov[0] = g_submitFov[1] = RenderFovFor(g_views[pick].fov, whole);
            EngineSetVrFov(2.0f * std::atan(std::tan(f.angleUp) * 16.0f / 9.0f) / kDeg);
            // Each eye rendered with exactly its own (asymmetric) field of view.
            bool off = Cfg().offCenter;
            for (int e = 0; e < 2; ++e) {
                if (off) {
                    g_renderFov[e] = OffCenterFovFor(g_views[e].fov, cov);
                    g_submitFov[e] = OffCenterFovFor(g_views[e].fov, whole);
                }
                const XrFovf& o = g_renderFov[e];
                EngineSetEyeProjection(e, std::tan(-o.angleLeft), std::tan(o.angleRight), std::tan(o.angleUp),
                                       std::tan(-o.angleDown), off);
                static float logged[2] = { -1, -1 };
                if (std::abs(logged[e] - o.angleLeft) > 0.001f) {
                    logged[e] = o.angleLeft;
                    Log("XR: eye %d renders %s fov: tan left %.3f right %.3f up %.3f down %.3f (%dx%d)", e,
                        off ? "off-centre" : "symmetric", std::tan(-o.angleLeft), std::tan(o.angleRight),
                        std::tan(o.angleUp), std::tan(-o.angleDown), g_bbW, g_bbH);
                }
            }
            g_renderPose[0] = ToPose(g_views[0].pose);
            g_renderPose[1] = ToPose(g_views[1].pose);
        }
        g_requestEye = eye;
        g_requestedThisFrame = true;
        eyeRc = eyeRcOf(eye);
    } else {
        if (g_requestedThisFrame) {
            eye = g_requestEye;
        } else {
            eye = g_leftDone ? 1 : 0;
            g_requestEye = eye;
            g_requestedThisFrame = true;
        }
        g_renderFov[eye] = RenderFovFor(g_views[eye].fov, FovCoverage());
        g_submitFov[eye] = RenderFovFor(g_views[eye].fov, Cfg().fovCrop ? 1.0f : FovCoverage());   // (CropRect)
        g_renderPose[eye] = ToPose(g_views[eye].pose);
        eyeRc = eyeRcOf(eye);
    }
    g_requestActive = true;
    HandState hl = g_handsRaw[0], hr = g_handsRaw[1];
    hl.aim = Mul(inv, hl.aim);
    hl.grip = Mul(inv, hl.grip);
    hr.aim = Mul(inv, hr.aim);
    hr.grip = Mul(inv, hr.grip);
    // Our floating panels: a grip pressed on one is the panel's until released, and the trigger of
    // the hand moving one does nothing (UpdatePanelGrips, decided under this lock with the controller
    // state read here): Lua sees neither (with the grip above 25 %, the tool copy and the free hand
    // would turn "ghost", main.lua; the trigger would fire / crouch there too). Nor that grip still
    // rising to 0.6 while its ray is on a panel it may take (the press about to be the panel's),
    // nor opening again after the panel's press until nearly open (g_gripQuiet).
    for (int i = 0; i < 2; ++i) {
        HandState& h = i ? hr : hl;
        const char* refused = nullptr;
        if (g_gripOwned[i] || g_gripQuiet[i] ||
            (!g_hold.gripDown[i] && h.squeeze <= 0.6f && h.squeeze > 0 && GripTarget(i, &refused).panel != kPanelNone &&
             !refused))
            h.squeeze = 0;
        if (g_trigMasked[i]) h.trigger = 0;
    }
    // Mods' interactive windows (engine.cpp, ModUiPointer): pointed at with either hand. A hand
    // on one of our own panels (the settings; the VR inventory while the player looks toward it),
    // or moving one, is not a pointer: each hand's own ray is tested (whichever hand drives those panels).
    if (!frozen) {
        bool ok[2];
        const float mpp = Inventory::kMetersPerPixel * Cfg().uiScale;
        for (int i = 0; i < 2; ++i) {
            const HandState& raw = g_handsRaw[i];   // (app space, like the panels' poses)
            float iu = 0, iv = 0;
            bool inside = false;
            const bool onInv = Inventory::Visible() && g_invFocus &&
                               RayPlane(raw.aim, g_invPose, Inventory::PanelW() * mpp, Inventory::PanelH() * mpp, iu,
                                        iv, inside) && inside;
            ok[i] = raw.active && !Ui::Visible() && !onInv && g_holdHand != i;
        }
        EngineSetUiRays(hl.aim, ok[0], hl.trigger, hr.aim, ok[1], hr.trigger);
    }
    // The off hand's laser stops on our panels (" LAS ...", see OffLaserStop).
    const int offIdx = Cfg().leftHanded ? 1 : 0;
    const float offStop = OffLaserStop(offIdx, offIdx ? hr.aim : hl.aim, eyeRcOf(0), eyeRcOf(1));
    // A stick used as the mouse (a mod's camera) does not also walk / turn in Lua.
    Input::MaskMouseStick(hl, hr);
    // (+ " INV ...": the VR inventory's fields, found by their tag at the end)
    return BuildResponse(req.seq, eye, g_renderFov[eye], eyeRc, headRc, hl, hr, frozen, offStop) +
           Inventory::ResponseFields();
}

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------
const char* VrText::c_str() const { return Tr(en.c_str(), fr.c_str()); }

const VrStatus& VrGetStatus() { return g_status; }

void VrRequestRecenter()
{
    g_recenterPending = true;
}

bool VrGetHands(HandState& left, HandState& right)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_session || !g_running) return false;
    Pose inv = Inverse(g_recenter);
    left = g_handsRaw[0];
    right = g_handsRaw[1];
    left.aim = Mul(inv, left.aim);
    left.grip = Mul(inv, left.grip);
    right.aim = Mul(inv, right.aim);
    right.grip = Mul(inv, right.grip);
    return true;
}

bool VrGetHead(Pose& head)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_session || !g_running) return false;
    head = Mul(Inverse(g_recenter), g_headRaw);
    return true;
}

void VrHaptic(int hand, float amplitude, float seconds)
{
    if (!g_session || !g_actionSet) return;
    XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
    vib.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vib.duration = (XrDuration)(seconds * 1e9);
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = aHaptic;
    hi.subactionPath = g_handPath[hand & 1];
    xrApplyHapticFeedback(g_session, &hi, (XrHapticBaseHeader*)&vib);
}

void VrPlaceUiPanel() { g_uiPlacePending = true; }
bool VrUiPanelVisible() { return Ui::Visible() && g_running; }

bool VrPanelOwnsGrip(int hand) { return g_gripOwned[hand & 1]; }
bool VrPanelMasksTrigger(int hand) { return g_trigMasked[hand & 1]; }
int VrPanelHoldHand() { return g_holdHand; }

// (present thread: Input::Update, after UpdatePanels measured the angles this present)
bool VrInventoryRecall()
{
    if (!Inventory::Visible() || !Inventory::Interactive() || g_hold.panel == kPanelInventory) return false;
    // (put back in front, it must come clearly closer to the gaze: looking at the sky with the
    // panel at its place, or at a panel parked aside on purpose, R3 still hides it)
    if (!(g_invGazeDeg > kRecallDeg && g_invFrontGazeDeg < g_invGazeDeg - 10)) return false;
    static int logs = 0;
    if (logs < 50) {
        ++logs;
        Log("Panels: R3 brings the inventory back in front of the body (it was %.0f deg from the gaze, %.0f once back)",
            g_invGazeDeg, g_invFrontGazeDeg);
    }
    return true;
}
