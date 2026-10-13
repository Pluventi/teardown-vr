// Sharper temporal anti-aliasing for VR.
//
// Teardown's TAA (compute shader "temporalaacs") reads the previous frame with a
// bilinear fetch at (pixel - motion) and keeps 90-95 % of it. In a headset the
// head is never perfectly still, so the history is re-filtered by a sub-pixel
// offset every frame and fine detail - mostly in the distance - blurs, then
// sharpens again during fast head turns. The replacement (shaders/
// temporalaacs_vr.hlsl, an exact rebuild of the game's shader) reads the history
// with a Catmull-Rom filter instead, which keeps it sharp.
//
// The engine binds the pipeline stored in its TAA wrapper object on every
// dispatch, so the replacement is simply swapped in there; the original is kept
// and put back when the setting is turned off. Offsets: see docs (section 6 ter).
#include "taa_vr.h"
#include "config.h"
#include "frameb.h"
#include "log.h"

#include <windows.h>
#include <d3d12.h>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#ifndef TDVR_NO_GAME_SHADERS
#include "generated/taa_net.h"
#include "generated/taa_natural.h"
#include "generated/taa_sharp.h"
#include "generated/taa_far.h"
#include "generated/taa_raw.h"
#endif

namespace {

constexpr uint32_t kTaaCompute = 0x40;   // TemporalAa -> compute pipeline wrapper ("temporalaacs")
constexpr uint32_t kWrapName = 0x10;     // wrapper -> name (inline)
constexpr uint32_t kWrapPso = 0x60;      // wrapper -> ID3D12PipelineState*
constexpr uint32_t kWrapRootSig = 0x68;  // wrapper -> ID3D12RootSignature*

struct State
{
    BYTE* wrapper = nullptr;
    ID3D12PipelineState* orig = nullptr;    // the game's pipeline (one reference held by us)
    ID3D12PipelineState* vr[10] = {};       // [1] net, [2] natural, [3] sharp, [4] maximum, [8] raw (test), [9] test file (references held by us)
    bool failed[10] = {};
    int logged = -1;
} g;

// What the last TaaVrUpdate left in the wrapper (TaaVrBoundMode): -1 the game's own pipeline (taaMode 0, ours could
// not be created, or no "temporalaacs" wrapper this render), else the taaMode whose VR shader is bound (1-4, 8, 9).
// Kept outside State: a new wrapper resets State, not what is bound.
int g_boundMode = -1;

bool IsOurs(ID3D12PipelineState* p)
{
    if (!p) return false;
    if (p == g.orig) return true;
    for (auto* v : g.vr)
        if (p == v) return true;
    return false;
}

// Development only (taaMode=9 in the ini): the shader is read from
// TeardownVR_taa_test.cso next to the game, to compare variants without rebuilding.
std::vector<char> g_testBlob;

ID3D12PipelineState* Create(int mode, ID3D12PipelineState* cur, ID3D12RootSignature* rs)
{
    ID3D12Device* dev = nullptr;
    if (FAILED(cur->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = rs;
#ifdef TDVR_NO_GAME_SHADERS
    // (built without the game-derived shaders: the game's anti-aliasing stays - see CMakeLists.txt)
    if (mode >= 1 && mode <= 8) {
        dev->Release();
        static bool logged = false;
        if (!logged) Log("TAA: this build has no VR anti-aliasing shaders (TDVR_GAME_SHADERS=OFF) - the game's is kept");
        logged = true;
        return nullptr;
    }
    if (false) {}
#else
    if (mode == 1) d.CS = { g_TaaNet, sizeof(g_TaaNet) };
    else if (mode == 2) d.CS = { g_TaaNatural, sizeof(g_TaaNatural) };
    else if (mode == 3) d.CS = { g_TaaSharp, sizeof(g_TaaSharp) };
    else if (mode == 4) d.CS = { g_TaaFar, sizeof(g_TaaFar) };
    else if (mode == 8) d.CS = { g_TaaRaw, sizeof(g_TaaRaw) };
#endif
    else {
        std::ifstream f(ModDirectory() + L"TeardownVR_taa_test.cso", std::ios::binary);
        g_testBlob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (g_testBlob.empty()) { dev->Release(); Log("TAA: test shader file missing"); return nullptr; }
        d.CS = { g_testBlob.data(), g_testBlob.size() };
    }
    ID3D12PipelineState* p = nullptr;
    HRESULT hr = dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&p));
    dev->Release();
    Log("TAA: VR anti-aliasing shader %d %s (hr=0x%08lx)", mode, SUCCEEDED(hr) ? "created" : "FAILED", (unsigned long)hr);
    return SUCCEEDED(hr) ? p : nullptr;
}

} // namespace

void TaaVrUpdate(unsigned char* taa)
{
    g_boundMode = -1;   // (until a VR pipeline is known to be in the wrapper below)
    if (!taa) return;
    BYTE* w = *(BYTE**)(taa + kTaaCompute);
    if (!w || memcmp(w + kWrapName, "temporalaacs", 13) != 0) return;
    auto** slot = (ID3D12PipelineState**)(w + kWrapPso);
    ID3D12PipelineState* cur = *slot;
    auto* rs = *(ID3D12RootSignature**)(w + kWrapRootSig);
    if (!cur || !rs) return;
    if (w != g.wrapper || !IsOurs(cur)) {
        // New TAA object (renderer rebuilt) or the game made a new pipeline: start
        // over. Old pipelines are deliberately not released (the GPU may still use them).
        g = State{};
        g.wrapper = w;
        g.orig = cur;
        cur->AddRef();
    }
    int mode = Cfg().taaMode;
    if (mode != 9 && (mode < 0 || mode > 4)) mode = 2;
    // (TEST ONLY: the control file TeardownVR_perf.txt "taaMode=N" - 0..4, or 8 = no anti-aliasing at all, each pixel's
    // own current sample (shaders/temporalaacs_vr.hlsl RAW_CURRENT=1): the mock test of stereoParticles measures the
    // per-frame grain of the particles and the lighting on it. Never offered in the settings panel; gone with the file.)
    const int fileMode = FrameB::FileTaaMode();
    if (fileMode >= 0 && (fileMode <= 4 || fileMode == 8)) mode = fileMode;
    ID3D12PipelineState* want = g.orig;
    if (mode > 0) {
        if (!g.vr[mode] && !g.failed[mode]) {
            g.vr[mode] = Create(mode, cur, rs);
            g.failed[mode] = !g.vr[mode];
        }
        if (g.vr[mode]) want = g.vr[mode];
    }
    if (g.logged != mode) {
        g.logged = mode;
        static const char* kNames[] = { "game's own anti-aliasing", "VR anti-aliasing: net", "VR anti-aliasing: natural",
                                        "VR anti-aliasing: sharp", "VR anti-aliasing: maximum" };
        Log("TAA: %s", want == g.orig ? kNames[0] : mode <= 4 ? kNames[mode] :
            mode == 8 ? "TEST - no anti-aliasing, the raw current frame (control file taaMode=8)" : "test shader from file");
    }
    g_boundMode = want == g.orig ? -1 : mode;   // (what the wrapper holds once this function returns)
    if (cur == want) return;
    want->AddRef();    // the wrapper now owns a reference to it
    *slot = want;
    cur->Release();    // the wrapper's reference to the previous one (we still hold ours)
}

int TaaVrBoundMode()
{
    return g_boundMode;
}
