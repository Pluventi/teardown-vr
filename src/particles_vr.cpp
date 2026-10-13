// Particles (and their lighting) drawn alike in both eyes ([Affichage] stereoParticles, default on).
//
// Teardown draws every particle (rain, smoke, fire, the extinguisher, dust) opaque into the G-buffer with stochastic
// transparency: the pixel shader "gbufferparticles" keeps a pixel when frac(blue noise + 0.618 * FrameRnd) is below
// the particle's alpha, else discards it, and the TAA blends the frames into a transparency. The blue noise texel is
// taken at the SCREEN PIXEL. In a headset the same point of a smoke cloud is ~730 px apart in the two eye images (the
// off-centre fields of view, plus its disparity), so each eye drew an unrelated dot pattern on it: the eyes cannot fuse
// two different fine textures (binocular rivalry: shimmer, eye strain). stereoSameNoise already gives both eyes the
// same FrameRnd; this module makes the right eye take every screen-tied random choice where the LEFT eye sees the same
// 3D point:
//  - the dither (round 1) and the front/back lighting checkerboard (round 2: every other pixel's normal is flipped to
//    light the dot from the back; an odd disparity lit a whole particle from the front in one eye and from the back in
//    the other) of shaders/gbufferparticles_vr.hlsl;
//  - the noise of the ambient occlusion rays and of the sun's shadow ray (round 2), shaders/
//    ambientdirectionallight_vr.hlsl: the lighting pass of every G-buffer pixel, the particle dots included (the light
//    denoiser keeps little of a dot's noise: it rejects other depths and normals).
//  Each VR shader is the game's (rebuilt from its DXIL, the lighting one byte-identical to the game's 16 blobs when the
//  VR change is off; checked on the GPU against the game's own blobs, every output bit, with GPU test harnesses that are
//  not published) running the game's formula on the left eye's pixel that shows this point:
//  this pixel moved by shift(w) = (cx / w + bx, by) (UV units, w = the right eye's clip w of the point), snapped to the
//  pixel grid. With a zero shift (the left eye) it is the game's shader bit for bit. Nothing else changes.
//  - The shift lives in the 12 padding bytes of the scene constants (SceneDynamicBuffer is 1F4h bytes; its buffer and
//    constant buffer view are 200h: the engine rounds constant buffers up to 100h). No shader of the game reads them;
//    the hook of the backend's buffer upload (only the scene constants' call) uploads 200h bytes: 0 in frame A (and
//    whenever no VR pipeline is bound), in frame B the left eye's reprojection, fitted from frame A's and frame B's
//    view-projection matrices (mubVpMatrix, both jittered: the pixel grids as rasterised).
//  - The pipelines: the engine creates each one once, at renderer start, in createGfxPSO (+5ae850): that function is
//    hooked (installed from the first DXGI factory creation, before the renderer exists) and, for "gbufferparticles" and
//    "ambientdirectionallight" whose pixel shader is a 2.0.1 blob one of ours was built from (container hash), calls
//    itself a second time with our pixel shader: a twin with the game's own blend, raster, depth, formats, input layout
//    and root signature. Before each Renderer::render the program objects (Renderer +dc0 / +dc8 / +dd0 / +dd8 for the
//    particles, +c38 / +c40 for the lighting) get the VR or the game's pipeline (+70h), as taa_vr.cpp does for the TAA:
//    the setting acts at once.
//  - The front/back checkerboard's parity in the right eye ([Affichage] stereoParity, round 3): round 2 takes it at
//    each particle layer's left-eye pixel. In dense smoke two neighbouring right-eye pixels that show layers at
//    different depths then get unrelated parities - a patchwork of the checkerboard and its inverse - and the VR TAA's
//    GLASS_FIX (taaMode 2, 3, 4: each particle pixel mixed 0.5 / 0.125 with its 4 neighbours, which cancels a screen
//    checkerboard exactly) cannot cancel it: the right eye alone kept an extra mottled grain. So each particle twin has
//    3 VR pipelines, one per STEREO_PARITY of shaders/gbufferparticles_vr.hlsl: 0 round 2, 1 the right eye's own
//    pixel's parity for particles of 8 px or more in radius (rain keeps round 2's), 2 its own pixel's for all. They
//    differ only by that parity, so the left eye (noisePixel = its own pixel) is the same in all three. Auto (-1, the
//    default): 1 when the VR TAA bound for this render is one of the GLASS_FIX modes (TaaVrBoundMode), else 0; the
//    control file's "stereoParity=N" overrides the ini (tests pin it: auto would switch between raw and TAA captures).
// RE: docs/REVERSE_ENGINEERING.md, section "Particules : la même trame dans les deux yeux".
#include "particles_vr.h"
#include "config.h"
#include "frameb.h"
#include "hooks.h"
#include "log.h"
#include "taa_vr.h"

#include <windows.h>
#include <d3d12.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef TDVR_NO_GAME_SHADERS
#include "generated/ambient_vr.h"
#include "generated/particles_p0.h"
#include "generated/particles_p1.h"
#include "generated/particles_p0_parity.h"
#include "generated/particles_p1_parity.h"
#endif

#pragma intrinsic(_ReturnAddress)

namespace {

// --- Teardown 2.0.1 ------------------------------------------------------------------------------------------------
// TRendererD3D12::createGfxPSO (+5ae850; its only caller is createGraphicsProgram +5afd50, vtable +180):
//   bool (backend, const ProgramDesc* desc, layout, const Blobs* blobs, ID3D12RootSignature* rs, ID3D12PipelineState** out)
// builds the D3D12_GRAPHICS_PIPELINE_STATE_DESC from desc and calls ID3D12Device::CreateGraphicsPipelineState
// (backend +e38, vtable +50) with VS = blobs +20h / +28h and PS = blobs +30h / +38h. It reads nothing else of blobs
// but its stage mask (+68h), and keeps no pointer to it.
constexpr const char* kCreatePsoPattern =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 A0 FC FF FF "
    "48 81 EC 60 04 00 00 4D 8B E1 4D 8B E8 48 8B DA 41 0F BC 41 68";
// ... mov rax,[r12+20h] ... [r12+28h] ... [r12+30h] ... mov eax,[r12+38h] ; mov rcx,[rbp+3A8h] (device) ;
// mov rax,[rcx] ; mov rdi,[rbp+3B8h] (out) ; mov r9,rdi ; lea r8,[IID] ; lea rdx,[rbp-80h] ; call [rax+50h]
constexpr const char* kCreatePsoCall =
    "49 8B 44 24 20 48 89 45 88 41 8B 44 24 28 48 89 45 90 49 8B 44 24 30 48 89 45 98 41 8B 44 24 38 48 89 45 A0 "
    "48 8B 8D A8 03 00 00 48 8B 01 48 8B BD B8 03 00 00 4C 8B CF 4C 8D 05 ?? ?? ?? ?? 48 8D 55 80 FF 50 50";
constexpr uint32_t kDescName = 0x70;          // ProgramDesc: the shader's name (engine string, 32 bytes)
constexpr uint32_t kDescPermutation = 0x90;   // ProgramDesc: the permutation ("GenerateMesh=1", "Samples2=1;...", "")
constexpr uint32_t kBlobsPs = 0x30;           // Blobs: pixel shader bytecode
constexpr uint32_t kBlobsPsSize = 0x38;       // Blobs: its size (32 bits)
constexpr uint32_t kBlobsSize = 0x70;         // Blobs: the part createGfxPSO reads
// Graphics program object (createGraphicsProgram, 80h bytes): its pipeline at +70h (set at each draw, +5b6c40, when
// it differs from the command list's last one) and root signature at +78h.
constexpr uint32_t kProgPso = 0x70, kProgRootSig = 0x78;
// The groups of twinned shaders: one switch each in the control file (test), both on with [Affichage] stereoParticles.
enum Group { kParticles = 0, kLighting = 1, kGroups = 2 };
constexpr const char* kGroupShader[kGroups] = { "gbufferparticles", "ambientdirectionallight" };
// Renderer (game +48h): the program objects of each group. The "gbufferparticles" programs (shader loader +93f00):
// +dc0 / +dc8 (CPU mesh; the second when the backend has its +a3 flag), +dd0 / +dd8 (GPU mesh, "GenerateMesh=1"); +9a7
// picks the second of a pair. The "ambientdirectionallight" programs (+9a2c0): +c38 (ambient occlusion only) and +c40
// ("DirectionalLight=1": with the sun), chosen per frame by the lighting pass (+8bf53: the sun's colour non-zero).
struct Slot { uint32_t off; Group group; };
constexpr Slot kRendPrograms[] = { { 0xdc0, kParticles }, { 0xdc8, kParticles }, { 0xdd0, kParticles },
                                   { 0xdd8, kParticles }, { 0xc38, kLighting }, { 0xc40, kLighting } };
constexpr uint32_t kRendTaa = 0xa30;          // TemporalAa: the render size at +10h / +14h (engine.cpp, per-eye history)
// TRendererD3D12::updateBuffer (vtable +128 = +5b9e00): (backend, buffer, data, size, offset, async) - copies data
// at once into the uploader (+10d8, or the async one +fc0 when async) and records the copy into the buffer.
constexpr const char* kUploadPattern =
    "48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 83 7C 24 78 00 41 8B E9 44 8B 7C 24 70 4D 8B F0 "
    "48 8B FA 48 8B F1 75 ??";
constexpr uint32_t kUploadSlot = 0x128;
// The scene constants (+9e130, Renderer::render +75e): lea r8,[rsp+30h] (the 1F4h bytes) ; xor edx,edx ;
// mov r9d,1F4h ; mov [rsp+28h],edx (sync) ; ... mov [rsp+20h],edx (offset 0) ; mov rdx,[rbx+0B90h] (the buffer) ;
// ... mov rax,[rcx] ; call [rax+128h]
constexpr const char* kScenePattern =
    "4C 8D 44 24 30 33 D2 41 B9 F4 01 00 00 89 54 24 28 0F 10 00 89 54 24 20 48 8B 93 90 0B 00 00";
constexpr const char* kSceneCall = "48 8B 01 FF 90 28 01 00 00";
constexpr uint32_t kSceneSize = 0x1f4, kScenePadded = 0x200;
// A buffer object (+5a9020): flags byte at +0 (4: constant buffer), size at +4 (rounded up to 100h for a constant
// buffer, the size of its view, +5df000), its allocation at +20h, whose ID3D12Resource is at +28h.
constexpr uint32_t kBufSize = 0x4, kBufAlloc = 0x20, kAllocResource = 0x28;

// The game's two particle pixel shaders our particle shader was rebuilt from (data/shader_cache.cache, Teardown 2.0.1):
// container hash (bytes 4..19) and size. Another blob (a game update) keeps the game's pipeline. (The 16 lighting ones
// are in generated/ambient_vr.h, g_AmbientVr.)
constexpr BYTE kOrigHash[2][16] = {
    { 0x65, 0x75, 0xbc, 0x20, 0x3a, 0xed, 0x5a, 0xda, 0x8d, 0x26, 0xb5, 0x44, 0x26, 0x07, 0x4c, 0xaf },
    { 0xfb, 0x54, 0x6f, 0xd0, 0x70, 0x80, 0xb6, 0xd7, 0xbd, 0xba, 0xc5, 0x96, 0x6a, 0x19, 0xc7, 0xa5 },
};
constexpr uint32_t kOrigSize[2] = { 8908, 10228 };

// The variants of our particle pixel shader (STEREO_PARITY of shaders/gbufferparticles_vr.hlsl): the right eye's
// front/back checkerboard - 0 the left eye's pixel's parity (round 2), 1 its own pixel's for particles of 8 px or more in
// radius, 2 its own pixel's for all. The lighting group has one shader (variant 0).
constexpr int kParities = 3;

// Our pixel shaders for a game blob of that group (data[0] nullptr when we have none: another game version).
struct VrBlob { const void* data[kParities] = {}; uint32_t size[kParities] = {}; int variants = 0; int perm = -1; };
VrBlob FindVrBlob(Group group, const BYTE* ps, uint32_t psSize)
{
    VrBlob b;
    if (!ps || psSize < 20) return b;
#ifdef TDVR_NO_GAME_SHADERS
    // (built without the game-derived shaders: no VR pipeline, the game's are kept - see CMakeLists.txt)
    (void)group;
    return b;
#else
    if (group == kParticles) {
        for (int i = 0; i < 2; ++i)
            if (psSize == kOrigSize[i] && memcmp(ps + 4, kOrigHash[i], 16) == 0) {
                b.data[0] = i ? (const void*)g_ParticlesVrP1 : (const void*)g_ParticlesVrP0;
                b.size[0] = i ? (uint32_t)sizeof(g_ParticlesVrP1) : (uint32_t)sizeof(g_ParticlesVrP0);
                b.data[1] = i ? (const void*)g_ParticlesVrP1Parity1 : (const void*)g_ParticlesVrP0Parity1;
                b.size[1] = i ? (uint32_t)sizeof(g_ParticlesVrP1Parity1) : (uint32_t)sizeof(g_ParticlesVrP0Parity1);
                b.data[2] = i ? (const void*)g_ParticlesVrP1Parity2 : (const void*)g_ParticlesVrP0Parity2;
                b.size[2] = i ? (uint32_t)sizeof(g_ParticlesVrP1Parity2) : (uint32_t)sizeof(g_ParticlesVrP0Parity2);
                b.variants = kParities;
                b.perm = i;
            }
    } else {
        for (int i = 0; i < 16; ++i)
            if (psSize == g_AmbientVr[i].gameSize && memcmp(ps + 4, g_AmbientVr[i].gameHash, 16) == 0) {
                b.data[0] = g_AmbientVr[i].vr;
                b.size[0] = g_AmbientVr[i].size;
                b.variants = 1;
                b.perm = i;
            }
    }
    return b;
#endif
}

using PFN_CreatePso = bool(__fastcall*)(void* backend, BYTE* desc, void* layout, BYTE* blobs, ID3D12RootSignature* rs,
                                        ID3D12PipelineState** out);
using PFN_Upload = uint64_t(__fastcall*)(void* backend, BYTE* buffer, const void* data, uint32_t size, uint32_t offset,
                                         uint32_t async);
PFN_CreatePso Real_CreatePso = nullptr;
PFN_Upload Real_Upload = nullptr;

struct Twin
{
    ID3D12PipelineState* orig = nullptr;   // the game's pipeline (one reference held by us)
    // ours (one reference each held by us): [0] always (the particles' STEREO_PARITY=0, the lighting's only one);
    // [1], [2] the particles' STEREO_PARITY=1 / 2, nullptr when not created ([0] is bound in their place)
    ID3D12PipelineState* vr[kParities] = {};
    ID3D12RootSignature* rs = nullptr;
    Group group = kParticles;
    int perm = 0;                          // the game blob's permutation index
};
// (each renderer build makes 6: 4 particle + 2 lighting pipelines - 14 VR pipelines with the particles' 3 variants;
// the renderer is rebuilt when some graphics options change. Entries are kept - and their pipelines referenced - for
// the game's lifetime: a program object may still hold the pipeline, and the GPU may still use it.)
constexpr int kMaxTwins = 96;
Twin g_twin[kMaxTwins];
int g_twins = 0;
int g_groupTwins[kGroups] = {};
SRWLOCK g_lock = SRWLOCK_INIT;
SRWLOCK g_earlyLock = SRWLOCK_INIT;   // ParticlesVrEarly: DXGI factories can be made on several threads at once

struct Fit
{
    float cx = 0, bx = 0, by = 0;   // shift(w) = (cx / w + bx, by), UV units
    double errPx = 0;               // largest error of that model against the exact reprojection, pixels
};

struct State
{
    int early = 0;                  // 0 not tried yet / still trying, 1 hooks in place, -1 not available
    int tries = 0;
    const void* sceneRet = nullptr; // the scene constants' upload call: its return address
    bool uploadSeen = false;        // the upload hook extended the scene constants (the padding is ours)
    BYTE* roomBuf = nullptr;        // the scene buffer last checked, and whether it has the 200h bytes
    bool roomOk = false;
    int notMatched[kGroups] = {};   // shaders of another game version
    // this render (main thread, Hook_Render)
    bool in = false;
    int eye = -1;
    uint64_t frame = 0;
    DWORD tid = 0;
    int w = 0, h = 0;
    bool bound = false;             // at least one VR pipeline is bound for this render (it reads the shift)
    bool boundGroup[kGroups] = {};  // ... of each group
    // frame A of the pair
    float leftVp[16] = {};
    uint64_t leftFrame = 0;
    bool leftValid = false;
    // logs
    int mode = -1;                  // the groups wanted (bit 0 particles, bit 1 lighting)
    int modeUser = 0;               // ... as the user set it: the control file's stereoParticles=0..3, or the ini's 0 / 1
    bool modeFile = false;          // ... from the control file
    int parity = 0;                 // the particles' STEREO_PARITY bound for this render (Parity())
    int parityLogged = -1;          // (parity | its source << 2, as last logged; -1 none or the particles were off)
    int parityFallback = 0;         // particle programs bound with variant 0 because their variant could not be made
    int fallbackLogged = 0;
    uint64_t rightParity[kParities] = {};   // frame B renders with the particles' VR pipelines, per parity bound
    int lastBound[kGroups] = { -1, -1 };
    bool slotsLogged[kGroups] = {};
    bool noTwinLogged = false;      // (the "no VR pipeline" line, once)
    bool noHookLogged = false;      // (the "hooks never installed" line, once)
    ULONGLONG logAt = 0;
    uint64_t pairs = 0, shifted = 0, noLeft = 0, failed = 0, pairsLit = 0;
    double maxErr = 0;
    Fit last;
} g;

const char* GameStr(const BYTE* s)
{
    // the engine's string: inline when byte +1Fh is 0, else a pointer at +0 (null: empty, as +50e220 reads it)
    const char* p = s[0x1f] ? *(const char* const*)s : (const char*)s;
    return p ? p : "";
}

// --- byte patterns ("48 8B ?? ..."), in the exe's .text -------------------------------------------------------------
struct Image
{
    BYTE* base = nullptr;
    BYTE* text = nullptr;
    BYTE* textEnd = nullptr;
    BYTE* rdata = nullptr;
    BYTE* rdataEnd = nullptr;
};

bool GetImage(Image& im)
{
    im.base = (BYTE*)GetModuleHandleW(nullptr);
    if (!im.base) return false;
    auto nt = (IMAGE_NT_HEADERS*)(im.base + ((IMAGE_DOS_HEADER*)im.base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        BYTE* lo = im.base + sec->VirtualAddress;
        if (strncmp((const char*)sec->Name, ".text", 5) == 0) { im.text = lo; im.textEnd = lo + sec->Misc.VirtualSize; }
        if (strncmp((const char*)sec->Name, ".rdata", 6) == 0) { im.rdata = lo; im.rdataEnd = lo + sec->Misc.VirtualSize; }
    }
    return im.text && im.rdata;
}

int ParsePattern(const char* pat, BYTE* bytes, bool* any, int cap)
{
    int n = 0;
    for (const char* p = pat; *p && n < cap;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { any[n] = true; bytes[n++] = 0; p += 2; continue; }
        unsigned v = 0;
        sscanf_s(p, "%2x", &v);
        any[n] = false;
        bytes[n++] = (BYTE)v;
        p += 2;
    }
    return n;
}

bool MatchAt(const BYTE* p, const BYTE* bytes, const bool* any, int n)
{
    for (int i = 0; i < n; ++i)
        if (!any[i] && p[i] != bytes[i]) return false;
    return true;
}

// Every match of the pattern in [lo, hi): the first one in *first, the count returned.
int FindAll(BYTE* lo, BYTE* hi, const char* pat, BYTE** first, BYTE** second = nullptr)
{
    BYTE bytes[128];
    bool any[128];
    const int n = ParsePattern(pat, bytes, any, 128);
    int count = 0;
    if (first) *first = nullptr;
    if (second) *second = nullptr;
    if (n <= 0 || any[0] || hi - lo < n) return 0;
    for (BYTE* p = lo; p + n <= hi;) {
        p = (BYTE*)memchr(p, bytes[0], (size_t)(hi - n + 1 - p));
        if (!p) break;
        if (MatchAt(p, bytes, any, n)) {
            if (count == 0 && first) *first = p;
            if (count == 1 && second) *second = p;
            ++count;
        }
        ++p;
    }
    return count;
}

// The TRendererD3D12 vtable slot holding f: f must be referenced by exactly one qword of .rdata, slot +128 of a
// vtable whose RTTI type is TRendererD3D12 (read before lazyBeginFrame moves that slot to its stub: this runs at the
// first DXGI factory, the stubs at the first Present).
bool IsRendererSlot(const Image& im, const BYTE* f, uint32_t slot)
{
    int refs = 0;
    BYTE* at = nullptr;
    for (BYTE* p = im.rdata; p + 8 <= im.rdataEnd; p += 8)
        if (*(const BYTE* const*)p == f) { ++refs; at = p; }
    if (refs != 1) return false;
    BYTE** vt = (BYTE**)(at - slot);
    if ((BYTE*)vt - 8 < im.rdata) return false;
    const BYTE* col = vt[-1];   // RTTI complete object locator: signature 1, type descriptor RVA at +0Ch
    if (col < im.rdata || col + 0x18 > im.rdataEnd || *(const DWORD*)col != 1) return false;
    const char* name = (const char*)(im.base + *(const DWORD*)(col + 0x0c) + 0x10);
    return strncmp(name, ".?AVTRendererD3D12@@", 21) == 0;
}

// --- the twin pipelines -------------------------------------------------------------------------------------------
void MakeTwin(Group group, void* backend, BYTE* desc, void* layout, BYTE* blobs, ID3D12RootSignature* rs,
              ID3D12PipelineState* orig)
{
    const BYTE* ps = *(const BYTE* const*)(blobs + kBlobsPs);
    const uint32_t psSize = *(const uint32_t*)(blobs + kBlobsPsSize);
    const char* perm = GameStr(desc + kDescPermutation);
    const VrBlob ours = FindVrBlob(group, ps, psSize);
    if (!ours.data[0]) {
        ++g.notMatched[group];
        char hex[33] = {};
        for (int i = 0; ps && psSize >= 20 && i < 16; ++i) sprintf_s(hex + 2 * i, 3, "%02x", ps[4 + i]);
        Log("Particles: the game's %s shader (permutation \"%s\", %u bytes, hash %s) is not one the VR shaders were "
            "made from (Teardown 2.0.1) - the game's pipeline is kept", kGroupShader[group], perm, psSize, hex);
        return;
    }
    // One pipeline per variant of our pixel shader (the particles: the 3 STEREO_PARITY; the lighting: 1), each by the
    // engine's own function: the same pipeline description, only the pixel shader differs.
    ID3D12PipelineState* vr[kParities] = {};
    for (int v = 0; v < ours.variants; ++v) {
        alignas(16) BYTE copy[kBlobsSize];
        memcpy(copy, blobs, kBlobsSize);
        *(const void**)(copy + kBlobsPs) = ours.data[v];
        *(uint32_t*)(copy + kBlobsPsSize) = ours.size[v];
        ID3D12PipelineState* p = nullptr;
        if (!Real_CreatePso(backend, desc, layout, copy, rs, &p) || !p) {
            if (v == 0) {
                Log("Particles: the VR pipeline of %s p%d (\"%s\") could not be created - the game's is kept",
                    kGroupShader[group], ours.perm, perm);
                return;
            }
            // (a variant of the checkerboard's parity: that program gets variant 0, round 2's, when it is asked for)
            Log("Particles: the VR pipeline of %s p%d (\"%s\") with STEREO_PARITY=%d could not be created - "
                "stereoParity=%d binds STEREO_PARITY=0 (round 2) there instead", kGroupShader[group], ours.perm, perm, v, v);
            continue;
        }
        vr[v] = p;
    }
    orig->AddRef();
    bool kept = false;
    AcquireSRWLockExclusive(&g_lock);
    if (g_twins < kMaxTwins) {
        Twin& t = g_twin[g_twins];
        t.orig = orig;
        for (int v = 0; v < kParities; ++v) t.vr[v] = vr[v];
        t.rs = rs;
        t.group = group;
        t.perm = ours.perm;
        ++g_groupTwins[group];
        ++g_twins;
        kept = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!kept) {
        for (auto* p : vr)
            if (p) p->Release();
        orig->Release();
        Log("Particles: more than %d VR pipelines (the renderer was rebuilt many times) - the new %s one keeps the "
            "game's shader", kMaxTwins, kGroupShader[group]);
        return;
    }
    if (group == kParticles)
        Log("Particles: VR twin of the pipeline %s p%d (\"%s\") created next to the game's - the right eye's front/back "
            "checkerboard variants STEREO_PARITY=0 / 1 / 2: ok / %s / %s", kGroupShader[group], ours.perm, perm,
            vr[1] ? "ok" : "none (0 instead)", vr[2] ? "ok" : "none (0 instead)");
    else
        Log("Particles: VR twin of the pipeline %s p%d (\"%s\") created next to the game's", kGroupShader[group],
            ours.perm, perm);
}

bool __fastcall Hook_CreatePso(void* backend, BYTE* desc, void* layout, BYTE* blobs, ID3D12RootSignature* rs,
                               ID3D12PipelineState** out)
{
    const bool ok = Real_CreatePso(backend, desc, layout, blobs, rs, out);
    if (ok && out && *out && desc && blobs && rs) {
        const char* name = GameStr(desc + kDescName);
        for (int gr = 0; gr < kGroups; ++gr)
            if (strcmp(name, kGroupShader[gr]) == 0) MakeTwin((Group)gr, backend, desc, layout, blobs, rs, *out);
    }
    return ok;
}

// The twin whose game pipeline or one of whose VR pipelines (any variant: the slot may hold the one bound at a previous
// render, before stereoParity changed) is pso.
const Twin* FindTwin(ID3D12PipelineState* pso, ID3D12RootSignature* rs)
{
    const Twin* t = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_twins && !t; ++i) {
        const Twin& c = g_twin[i];
        if (c.rs != rs) continue;
        bool ours = c.orig == pso;
        for (int v = 0; v < kParities && !ours; ++v) ours = c.vr[v] && c.vr[v] == pso;
        if (ours) t = &c;
    }
    ReleaseSRWLockShared(&g_lock);
    return t;   // (entries are never removed nor moved)
}

// The VR pipeline of that twin for the particles' checkerboard parity (the lighting: its only one). A variant that
// could not be created gives variant 0 (round 2) and counts in g.parityFallback.
ID3D12PipelineState* VrPipeline(const Twin& t, int parity)
{
    if (t.group != kParticles || parity <= 0 || parity >= kParities) return t.vr[0];
    if (t.vr[parity]) return t.vr[parity];
    ++g.parityFallback;
    return t.vr[0];
}

// --- the left eye's reprojection ------------------------------------------------------------------------------------
// The game's matrices map a world point as clip = (x, y, z, 1) * M (row-major storage, the shaders' mul order).
// The point a right-eye fragment shows, from its NDC (nx, ny) and clip w: clip.x = nx w, clip.y = ny w, clip.w = w.
bool PointFor(const float* M, double nx, double ny, double w, double P[3])
{
    double A[3][3], b[3];
    for (int i = 0; i < 3; ++i) {
        A[0][i] = (double)M[i * 4 + 0] - nx * M[i * 4 + 3];
        A[1][i] = (double)M[i * 4 + 1] - ny * M[i * 4 + 3];
        A[2][i] = M[i * 4 + 3];
    }
    b[0] = -((double)M[12] - nx * M[15]);
    b[1] = -((double)M[13] - ny * M[15]);
    b[2] = w - M[15];
    const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                       A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;
    for (int c = 0; c < 3; ++c) {
        double T[3][3];
        memcpy(T, A, sizeof(T));
        for (int r = 0; r < 3; ++r) T[r][c] = b[r];
        P[c] = (T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) +
                T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0])) / det;
    }
    return true;
}

bool Project(const float* M, const double P[3], double& nx, double& ny, double& w)
{
    double c[4];
    for (int j = 0; j < 4; ++j) c[j] = P[0] * M[j] + P[1] * M[4 + j] + P[2] * M[8 + j] + M[12 + j];
    if (!(c[3] > 1e-6)) return false;
    w = c[3];
    nx = c[0] / c[3];
    ny = c[1] / c[3];
    return true;
}

// shift(w) = (cx / w + bx, by): where the left eye (L) shows the point a right-eye (R) fragment at clip w shows, minus
// that fragment's own place, in UV units (half NDC, y down). Exact for two eyes with the same orientation side by side
// (offset along the view's x: the 1/w term; off-centre fields of view: the constant terms); fitted on the right eye's
// central ray, then checked over its whole view from 0.4 m (particles fade out nearer) to 300 m.
bool FitShift(const float* L, const float* R, int w, int h, Fit& f)
{
    auto delta = [&](double nx, double ny, double wr, double& du, double& dv) {
        double P[3], lx, ly, wl;
        if (!PointFor(R, nx, ny, wr, P) || !Project(L, P, lx, ly, wl)) return false;
        du = (lx - nx) * 0.5;
        dv = -(ly - ny) * 0.5;
        return true;
    };
    double du1, dv1, du2, dv2;
    const double w1 = 1.0, w2 = 256.0;
    if (!delta(0, 0, w1, du1, dv1) || !delta(0, 0, w2, du2, dv2)) return false;
    const double cx = (du1 - du2) / (1.0 / w1 - 1.0 / w2);
    const double bx = du2 - cx / w2;
    const double by = dv2;
    double err = 0;
    for (double nx : { -0.95, 0.0, 0.95 })
        for (double ny : { -0.95, 0.0, 0.95 })
            for (double wr : { 0.4, 1.0, 4.0, 30.0, 300.0 }) {
                double du, dv;
                if (!delta(nx, ny, wr, du, dv)) return false;
                err = std::max({ err, std::fabs(cx / wr + bx - du) * w, std::fabs(by - dv) * h });
            }
    if (!std::isfinite(cx) || !std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(err)) return false;
    // (not a stereo pair's two eyes: a shift over a whole image width, or a model off by more than 4 pixels)
    if (std::fabs(bx) > 1.0 || std::fabs(cx / 0.4) > 2.0 || std::fabs(by) > 1.0 || err > 4.0) return false;
    f.cx = (float)cx;
    f.bx = (float)bx;
    f.by = (float)by;
    f.errPx = err;
    return true;
}

// The groups wanted: bit 0 the particles (dither and front/back lighting), bit 1 the lighting (ambient occlusion and
// sun noise). [Affichage] stereoParticles: both or none. The control file (tests): "stereoParticles=N", 0 none, 1 both,
// 2 the particles only, 3 the lighting only.
int Mode()
{
    const int f = FrameB::FileStereoParticles();
    if (f >= 0) return f == 0 ? 0 : f == 2 ? 1 : f == 3 ? 2 : 3;
    return Cfg().stereoParticles ? 3 : 0;
}

// The particles' front/back checkerboard variant for this render (the STEREO_PARITY of the pipelines bound; only the
// right eye's image differs between them): the control file's "stereoParity=N" (0..2), else [Affichage] stereoParity
// (0..2), else auto (-1, or any other value): 1 when the VR anti-aliasing bound for this render (taa_vr.cpp, just
// before us in Hook_Render) is one with GLASS_FIX - taaMode 2 natural, 3 sharp, 4 maximum: built with SHARP_CURRENT=2
// GLASS_FIX=1, their DXIL (generated/taa_*.h) tests the mask with "and ..., 36" (kIsGlass | kIsParticles) and then
// mixes 0.5 x the pixel + 0.125 x each of its 4 neighbours, which cancels a screen checkerboard. Else 0: the game's own
// TAA, net (1), raw (8, test) and the test file (9) have no such filter, nothing cancels the checkerboard, and the own
// pixel's parity would bring back round 1's inversion (a large particle lit from the front in one eye and from the back
// in the other): round 2's matching dots are kept. source: 0 the ini, 1 the control file, 2 auto; taa: the bound
// taaMode the auto rule read (-1 the game's own).
int Parity(int& source, int& taa)
{
    taa = TaaVrBoundMode();
    const int f = FrameB::FileStereoParity();
    if (f >= 0 && f < kParities) { source = 1; return f; }
    const int c = Cfg().stereoParity;
    if (c >= 0 && c < kParities) { source = 0; return c; }
    source = 2;
    return taa == 2 || taa == 3 || taa == 4 ? 1 : 0;
}

const char* ParityText(int parity)
{
    switch (parity) {
    case 0: return "the right eye lights each particle dot from the side the left eye lights it (the left eye's pixel's "
                   "front/back parity, round 2)";
    case 1: return "the right eye takes its own pixel's front/back parity for particles of 8 px or more in radius (the VR "
                   "anti-aliasing's 4-neighbour filter then cancels its checkerboard as in the left eye), the left eye's "
                   "pixel's for thinner ones (rain)";
    default: return "the right eye takes its own pixel's front/back parity for every particle";
    }
}

const char* ModeText(int mode)
{
    switch (mode) {
    case 0: return "off - the game's particle and lighting shaders in both eyes (as before)";
    case 1: return "the particles only (test): the right eye takes each particle's dither and front/back lighting where "
                   "the left eye sees the same point; the lighting noise stays the game's";
    case 2: return "the lighting only (test): the right eye takes the ambient occlusion and sun noise where the left eye "
                   "sees the same point; the particles stay the game's";
    default: return "the right eye takes each particle's dither and front/back lighting, and the ambient occlusion and "
                    "sun noise, where the left eye sees the same point";
    }
}

void LogSummary()
{
    const Fit& f = g.last;
    // (stereoParticles= the value the user set - the control file's 0..3, or the ini's 0 / 1 -, not g.mode, the
    // groups' bitmask: the ini's 1 used to read "stereoParticles=3" here; what it means follows, ModeText)
    Log("Particles: stereoParticles=%d%s: %llu stereo pairs, the right eye took the left eye's noise in %llu (%llu "
        "without this pair's frame A, %llu not reprojectable; the lighting's in %llu); last: %+.1f px at infinity, %+.1f "
        "px x 1 m / distance, %+.2f px vertically, model error %.3f px (worst %.3f); right eye's front/back checkerboard "
        "stereoParity 0 / 1 / 2 in %llu / %llu / %llu renders - %s", g.modeUser, g.modeFile ? " (control file)" : "",
        (unsigned long long)g.pairs, (unsigned long long)g.shifted, (unsigned long long)g.noLeft,
        (unsigned long long)g.failed, (unsigned long long)g.pairsLit, f.bx * g.w, f.cx * g.w, f.by * g.h, f.errPx,
        g.maxErr, (unsigned long long)g.rightParity[0], (unsigned long long)g.rightParity[1],
        (unsigned long long)g.rightParity[2], ModeText(g.mode));
    g.pairs = g.shifted = g.noLeft = g.failed = g.pairsLit = 0;
    g.rightParity[0] = g.rightParity[1] = g.rightParity[2] = 0;
    g.maxErr = 0;
}

// The scene constants of this render: frame A keeps its matrix (and gets no shift), frame B gets the left eye's
// reprojection when a VR pipeline is bound.
void SceneShift(const float* vp, float shift[3])
{
    if (!g.in || GetCurrentThreadId() != g.tid || g.eye < 0) return;
    if (g.eye == 0) {
        // (a second render of the same frame A, if the game ever makes one: the pair keeps the first one's matrix, as
        // SameNoiseBegin keeps the first one's TAA counter)
        if (g.leftValid && g.leftFrame == g.frame) return;
        memcpy(g.leftVp, vp, sizeof(g.leftVp));
        g.leftFrame = g.frame;
        g.leftValid = true;
        return;
    }
    if (!g.bound) return;
    ++g.pairs;
    if (!g.leftValid || g.leftFrame + 1 != g.frame) { ++g.noLeft; return; }
    Fit f;
    if (!FitShift(g.leftVp, vp, std::max(g.w, 1), std::max(g.h, 1), f)) { ++g.failed; return; }
    shift[0] = f.cx;
    shift[1] = f.bx;
    shift[2] = f.by;
    ++g.shifted;
    if (g.boundGroup[kLighting]) ++g.pairsLit;
    g.last = f;
    g.maxErr = std::max(g.maxErr, f.errPx);
}

bool RoomFor(BYTE* buf)
{
    if (buf == g.roomBuf) return g.roomOk;
    g.roomBuf = buf;
    g.roomOk = false;
    const uint32_t size = *(const uint32_t*)(buf + kBufSize);
    BYTE* alloc = *(BYTE**)(buf + kBufAlloc);
    ID3D12Resource* res = alloc ? *(ID3D12Resource**)(alloc + kAllocResource) : nullptr;
    UINT64 width = 0;
    if (buf[0] == 4 && size >= kScenePadded && res) {
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) width = d.Width;
    }
    g.roomOk = width >= kScenePadded;
    Log("Particles: scene constants buffer: view %u bytes, resource %llu bytes - %s", size, (unsigned long long)width,
        g.roomOk ? "room for the VR shift (bytes 500..511)" : "NO room for the VR shift: the game's shaders are kept");
    return g.roomOk;
}

uint64_t __fastcall Hook_Upload(void* backend, BYTE* buffer, const void* data, uint32_t size, uint32_t offset, uint32_t async)
{
    if (_ReturnAddress() != g.sceneRet || size != kSceneSize || offset != 0 || !data || !buffer || !RoomFor(buffer))
        return Real_Upload(backend, buffer, data, size, offset, async);
    alignas(16) BYTE ext[kScenePadded];
    memcpy(ext, data, kSceneSize);
    float shift[3] = { 0, 0, 0 };
    SceneShift((const float*)ext, shift);   // (mubVpMatrix at +0)
    memcpy(ext + kSceneSize, shift, sizeof(shift));
    g.uploadSeen = true;
    return Real_Upload(backend, buffer, ext, kScenePadded, 0, async);
}

} // namespace

// (ParticlesVrEarly, under g_earlyLock, while g.early is 0)
static void EarlyTry()
{
    Image im;
    if (!GetImage(im)) return;
    BYTE *pso = nullptr, *pso2 = nullptr, *call = nullptr, *scene = nullptr, *up1 = nullptr, *up2 = nullptr;
    const int nPso = FindAll(im.text, im.textEnd, kCreatePsoPattern, &pso, &pso2);
    if (nPso == 0) {
        // (the code may still be encrypted by the Steam wrapper if a factory is made before the game runs: retry)
        if (++g.tries >= 16) {
            g.early = -1;
            Log("Particles: the engine's pipeline creation not found (game version?) - stereoParticles unavailable");
        }
        return;
    }
    const int nCall = pso ? FindAll(pso + 0x600, pso + 0x680, kCreatePsoCall, &call) : 0;
    const int nScene = FindAll(im.text, im.textEnd, kScenePattern, &scene);
    BYTE* sceneCall = nullptr;
    if (nScene == 1) FindAll(scene + 0x30, scene + 0x80, kSceneCall, &sceneCall);
    const int nUp = FindAll(im.text, im.textEnd, kUploadPattern, &up1, &up2);
    BYTE* upload = up1 && IsRendererSlot(im, up1, kUploadSlot) ? up1 : up2 && IsRendererSlot(im, up2, kUploadSlot) ? up2 : nullptr;
    if (nPso != 1 || nCall != 1 || !sceneCall || !upload) {
        g.early = -1;
        Log("Particles: engine code not recognised (pipeline creation %d / %d, scene constants %d / %d, buffer upload %d / "
            "%d) - stereoParticles unavailable", nPso, nCall, nScene, sceneCall ? 1 : 0, nUp, upload ? 1 : 0);
        return;
    }
    g.sceneRet = sceneCall + 9;
    if (!HookFunction(pso, (void*)Hook_CreatePso, (void**)&Real_CreatePso) ||
        !HookFunction(upload, (void*)Hook_Upload, (void**)&Real_Upload)) {
        g.early = -1;
        Log("Particles: hooks NOT installed - stereoParticles unavailable");
        return;
    }
    g.early = 1;
    Log("Particles: hooks ready before the renderer starts (pipeline creation +%llx, buffer upload +%llx, scene constants' "
        "call +%llx); [Affichage] stereoParticles=%d", (unsigned long long)(pso - im.base),
        (unsigned long long)(upload - im.base), (unsigned long long)(sceneCall - im.base), Cfg().stereoParticles ? 1 : 0);
}

bool GameShadersBuilt()
{
#ifdef TDVR_NO_GAME_SHADERS
    return false;
#else
    return true;
#endif
}

void ParticlesVrEarly()
{
#ifdef TDVR_NO_GAME_SHADERS
    // (built without the game-derived shaders: nothing to hook - one line, once)
    static bool logged = false;
    if (!logged) Log("Particles: this build has no stereo-particle shaders (TDVR_GAME_SHADERS=OFF) - the game's are kept");
    logged = true;
    return;
#endif
    AcquireSRWLockExclusive(&g_earlyLock);
    if (g.early == 0) EarlyTry();
    ReleaseSRWLockExclusive(&g_earlyLock);
}

void ParticlesVrBeginRender(unsigned char* r, int eye, uint64_t frame)
{
    g.in = true;
    g.eye = eye;
    g.frame = frame;
    g.tid = GetCurrentThreadId();
    g.bound = false;
    g.boundGroup[kParticles] = g.boundGroup[kLighting] = false;
    if (r && g.early == 0 && !g.noHookLogged) {
        // The renderer runs and the hooks never went in: no DXGI factory was made with the engine's code recognised
        // before it built its shaders. Too late now (its pipelines exist): no more tries.
        AcquireSRWLockExclusive(&g_earlyLock);
        if (g.early == 0) g.early = -1;
        ReleaseSRWLockExclusive(&g_earlyLock);
        g.noHookLogged = true;
        if (g.early == -1)
            Log("Particles: the engine's pipeline creation was not found before the renderer started (%d DXGI factories "
                "seen; game version?) - stereoParticles unavailable", g.tries);
    }
    if (!r || g.early != 1) return;
    if (BYTE* taa = *(BYTE**)(r + kRendTaa)) {
        g.w = *(const int*)(taa + 0x10);
        g.h = *(const int*)(taa + 0x14);
    }
    const int mode = Mode();
    const int fileMode = FrameB::FileStereoParticles();
    const int userMode = fileMode >= 0 ? fileMode : (mode ? 1 : 0);
    if (mode != g.mode || userMode != g.modeUser || (fileMode >= 0) != g.modeFile) {
        // (the summary of the period that ends, under its own setting; then the new setting)
        if (g.mode >= 0 && g.logAt) LogSummary();
        const int sameNoise = FrameB::FileSameNoise() >= 0 ? FrameB::FileSameNoise() : Cfg().stereoSameNoise;
        Log("Particles: stereoParticles=%d%s: %s%s", userMode, fileMode >= 0 ? " (control file)" : "",
            ModeText(mode), mode && !sameNoise ? " (stereoSameNoise=0: the eyes still draw different noise frames - set "
                                                 "it to 1)" : "");
        g.mode = mode;
        g.modeUser = userMode;
        g.modeFile = fileMode >= 0;
        g.logAt = GetTickCount64() + 5000;
    }
    // The particles' front/back checkerboard variant (Parity: stereoParity, the control file's or auto). The three
    // pipelines give the left eye the same image, so it is bound in both eyes alike (no switch inside a pair).
    int paritySource = 0, parityTaa = -1;
    g.parity = Parity(paritySource, parityTaa);
    g.parityFallback = 0;
    // No twin (the game's shaders are not 2.0.1's, or were made before the hooks): the renderer's program slots
    // (2.0.1 offsets) are not even read.
    if (g_twins == 0) {
        if (!g.noTwinLogged) {
            g.noTwinLogged = true;
            Log("Particles: no VR pipeline (the game's particle and lighting shaders were made before the hooks, or are "
                "not 2.0.1's: %d / %d) - stereoParticles unavailable", g.notMatched[kParticles], g.notMatched[kLighting]);
        }
        return;
    }
    // The VR pipelines only once the scene constants carry our shift (the padding is then ours, 0 in frame A).
    int vr[kGroups] = {}, game[kGroups] = {}, other[kGroups] = {};
    for (const Slot& s : kRendPrograms) {
        if (g_groupTwins[s.group] == 0) continue;   // (a group with no twin: its 2.0.1 slots are not even read)
        BYTE* prog = *(BYTE**)(r + s.off);
        if (!prog) continue;
        auto** slot = (ID3D12PipelineState**)(prog + kProgPso);
        ID3D12PipelineState* cur = *slot;
        const Twin* t = cur ? FindTwin(cur, *(ID3D12RootSignature**)(prog + kProgRootSig)) : nullptr;
        if (!t || t->group != s.group) { ++other[s.group]; continue; }
        const bool want = (mode >> s.group & 1) && g.uploadSeen;
        // (the particles: the variant of g.parity; the lighting: its one VR pipeline)
        ID3D12PipelineState* target = want ? VrPipeline(*t, g.parity) : t->orig;
        if (cur != target) {
            target->AddRef();   // (the program object now owns a reference to it; we keep ours to both)
            *slot = target;
            cur->Release();
        }
        ++(want ? vr : game)[s.group];
    }
    g.boundGroup[kParticles] = vr[kParticles] > 0;
    g.boundGroup[kLighting] = vr[kLighting] > 0;
    g.bound = g.boundGroup[kParticles] || g.boundGroup[kLighting];
    for (int gr = 0; gr < kGroups; ++gr) {
        if (!g.slotsLogged[gr] && g_groupTwins[gr] > 0) {
            g.slotsLogged[gr] = true;
            if (vr[gr] + game[gr] == 0)
                Log("Particles: the renderer's %s programs do not use the pipelines made at start (%d twins, %d other) - "
                    "no effect on them", kGroupShader[gr], g_groupTwins[gr], other[gr]);
        }
        const int boundNow = vr[gr] > 0 ? 1 : 0;
        if (boundNow != g.lastBound[gr] && (vr[gr] || game[gr])) {
            g.lastBound[gr] = boundNow;
            if (gr == kParticles)
                Log(boundNow ? "Particles: stereo-anchored dither and front/back lighting on (gbufferparticles: %d VR "
                               "pipelines bound)"
                             : "Particles: the game's particle shader (%d pipelines)", boundNow ? vr[gr] : game[gr]);
            else
                Log(boundNow ? "Particles: stereo-anchored lighting noise on (ambientdirectionallight - ambient "
                               "occlusion and sun: %d VR pipelines bound)"
                             : "Particles: the game's lighting shader (ambientdirectionallight, %d pipelines)",
                    boundNow ? vr[gr] : game[gr]);
        }
    }
    // The checkerboard variant in use, logged when it (or where it comes from) changes while the particles' VR
    // pipelines are bound; again after they were off.
    if (vr[kParticles] > 0) {
        const int key = g.parity | paritySource << 2;
        if (key != g.parityLogged) {
            g.parityLogged = key;
            char why[96];
            if (paritySource == 1) snprintf(why, sizeof(why), "control file");
            else if (paritySource == 0) snprintf(why, sizeof(why), "ini");
            else if (parityTaa < 0) snprintf(why, sizeof(why), "auto: the game's own anti-aliasing, no GLASS_FIX");
            else snprintf(why, sizeof(why), "auto: VR anti-aliasing taaMode %d, %s GLASS_FIX", parityTaa,
                          g.parity ? "with" : "no");
            Log("Particles: stereoParity=%d (%s): %s", g.parity, why, ParityText(g.parity));
        }
        if (g.parityFallback != g.fallbackLogged) {
            g.fallbackLogged = g.parityFallback;
            if (g.parityFallback)
                Log("Particles: stereoParity=%d: %d particle program(s) have no such VR pipeline - STEREO_PARITY=0 "
                    "(round 2) bound there", g.parity, g.parityFallback);
        }
        if (eye == 1) ++g.rightParity[g.parity];
    } else {
        g.parityLogged = -1;
    }
    if (g.logAt && GetTickCount64() >= g.logAt) {
        if (g.pairs || g.mode) LogSummary();
        g.logAt = GetTickCount64() + 60000;
    }
}

void ParticlesVrEndRender()
{
    g.in = false;
}

int ParticlesVrStatus()
{
    if (g.early != 1 || g_twins == 0) return 0;
    return g.lastBound[kParticles] == 1 || g.lastBound[kLighting] == 1 ? 2 : 1;
}
