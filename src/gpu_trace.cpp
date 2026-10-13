// Reverse-engineering aid: records, for exactly one game frame, every D3D12
// command the game records (draws, dispatches, clears, copies, pipeline
// changes) and from where in teardown.exe it was issued. This maps out the
// renderer's passes and the function orchestrating them.
#include "gpu_trace.h"
#include "hooks.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace {

enum Method { M_Close, M_Reset, M_Draw, M_DrawIndexed, M_Dispatch, M_CopyTexture, M_CopyResource, M_SetPso,
              M_SetRT, M_ClearRT, M_ClearDS, M_ExecIndirect, M_Count };
const char* kNames[M_Count] = { "Close", "Reset", "Draw", "DrawIndexed", "Dispatch", "CopyTextureRegion",
                                "CopyResource", "SetPipelineState", "OMSetRenderTargets", "ClearRTV", "ClearDSV",
                                "ExecuteIndirect" };

std::mutex g_mutex;
int g_state = 0; // 0 idle, 1 armed, 2 tracing, 3 done
int g_index = 0;
std::map<ULONG_PTR, int> g_sites;     // callsite -> first index
int g_counts[M_Count] = {};
ID3D12Resource* g_watched[8] = {};
int g_watchedCount = 0;
bool g_verbose = true;    // log every command with its stack

ULONG_PTR CallSite()
{
    void* frames[8];
    USHORT n = RtlCaptureStackBackTrace(3, 8, frames, nullptr);
    auto base = (ULONG_PTR)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    ULONG_PTR end = base + nt->OptionalHeader.SizeOfImage;
    for (USHORT i = 0; i < n; ++i)
        if ((ULONG_PTR)frames[i] >= base && (ULONG_PTR)frames[i] < end) return (ULONG_PTR)frames[i] - base;
    return 0;
}

bool IsWatched(ID3D12Resource* r)
{
    for (int i = 0; i < g_watchedCount; ++i)
        if (g_watched[i] == r) return true;
    return false;
}

void Record(Method m)
{
    if (g_state != 2) return;
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(2, 24, frames, nullptr);
    auto base = (ULONG_PTR)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    ULONG_PTR end = base + nt->OptionalHeader.SizeOfImage;
    std::lock_guard<std::mutex> lock(g_mutex);
    int idx = g_index++;
    g_counts[m]++;
    ULONG_PTR site = 0;
    for (USHORT i = 0; i < n; ++i)
        if ((ULONG_PTR)frames[i] >= base && (ULONG_PTR)frames[i] < end) { site = (ULONG_PTR)frames[i] - base; break; }
    if (g_sites.count(site) && !g_verbose) return;
    g_sites[site] = idx;
    char line[600];
    int len = snprintf(line, sizeof(line), "GT #%d %s site=%llx tid=%lu stack:", idx, kNames[m],
                       (unsigned long long)site, GetCurrentThreadId());
    for (USHORT i = 0; i < n && len < (int)sizeof(line) - 16; ++i) {
        auto a = (ULONG_PTR)frames[i];
        if (a >= base && a < end) len += snprintf(line + len, sizeof(line) - len, " %llx", (unsigned long long)(a - base));
    }
    Log("%s", line);
}

#define HOOK_DECL(idx, name, ret, params, args, method)                                   \
    using PFN_##name = ret(STDMETHODCALLTYPE*) params;                                    \
    PFN_##name Real_##name;                                                               \
    ret STDMETHODCALLTYPE Hook_##name params { Record(method); return Real_##name args; }

HOOK_DECL(9, Close, HRESULT, (ID3D12GraphicsCommandList* c), (c), M_Close)
HOOK_DECL(10, Reset, HRESULT, (ID3D12GraphicsCommandList* c, ID3D12CommandAllocator* a, ID3D12PipelineState* p), (c, a, p), M_Reset)
HOOK_DECL(12, DrawInstanced, void, (ID3D12GraphicsCommandList* c, UINT a, UINT b, UINT d, UINT e), (c, a, b, d, e), M_Draw)
HOOK_DECL(13, DrawIndexedInstanced, void, (ID3D12GraphicsCommandList* c, UINT a, UINT b, UINT d, INT e, UINT f), (c, a, b, d, e, f), M_DrawIndexed)
HOOK_DECL(14, Dispatch, void, (ID3D12GraphicsCommandList* c, UINT a, UINT b, UINT d), (c, a, b, d), M_Dispatch)
using PFN_CopyTextureRegion = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
PFN_CopyTextureRegion Real_CopyTextureRegion;
void STDMETHODCALLTYPE Hook_CopyTextureRegion(ID3D12GraphicsCommandList* c, const D3D12_TEXTURE_COPY_LOCATION* a, UINT b, UINT d, UINT e, const D3D12_TEXTURE_COPY_LOCATION* f, const D3D12_BOX* g)
{
    if (g_state == 2) {
        D3D12_RESOURCE_DESC dd = a->pResource->GetDesc(), sd = f->pResource->GetDesc();
        Log("GT #%d CopyTextureRegion dst=%p (%llux%u fmt %d)%s <- src=%p (%llux%u fmt %d)%s site=%llx", g_index,
            (void*)a->pResource, dd.Width, dd.Height, dd.Format, IsWatched(a->pResource) ? " [BACKBUFFER]" : "",
            (void*)f->pResource, sd.Width, sd.Height, sd.Format, IsWatched(f->pResource) ? " [BACKBUFFER]" : "",
            (unsigned long long)CallSite());
    }
    Record(M_CopyTexture);
    Real_CopyTextureRegion(c, a, b, d, e, f, g);
}
HOOK_DECL(17, CopyResource, void, (ID3D12GraphicsCommandList* c, ID3D12Resource* a, ID3D12Resource* b), (c, a, b), M_CopyResource)
HOOK_DECL(25, SetPipelineState, void, (ID3D12GraphicsCommandList* c, ID3D12PipelineState* a), (c, a), M_SetPso)
HOOK_DECL(47, ClearDepthStencilView, void, (ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE a, D3D12_CLEAR_FLAGS b, FLOAT d, UINT8 e, UINT f, const D3D12_RECT* g), (c, a, b, d, e, f, g), M_ClearDS)
HOOK_DECL(48, ClearRenderTargetView, void, (ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE a, const FLOAT* b, UINT d, const D3D12_RECT* e), (c, a, b, d, e), M_ClearRT)
HOOK_DECL(59, ExecuteIndirect, void, (ID3D12GraphicsCommandList* c, ID3D12CommandSignature* a, UINT b, ID3D12Resource* d, UINT64 e, ID3D12Resource* f, UINT64 g), (c, a, b, d, e, f, g), M_ExecIndirect)

bool g_installed = false;

} // namespace

void GpuTraceInstall(ID3D12CommandList* anyList)
{
    if (g_installed || !anyList) return;
    if (anyList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return; // only the main graphics list
    ID3D12GraphicsCommandList* gl = nullptr;
    if (FAILED(anyList->QueryInterface(IID_PPV_ARGS(&gl)))) return;
    g_installed = true;
    void** vt = *(void***)gl;
#define HOOK_IT(idx, name) HookFunction(vt[idx], (void*)Hook_##name, (void**)&Real_##name)
    HOOK_IT(9, Close);
    HOOK_IT(10, Reset);
    HOOK_IT(12, DrawInstanced);
    HOOK_IT(13, DrawIndexedInstanced);
    HOOK_IT(14, Dispatch);
    HOOK_IT(16, CopyTextureRegion);
    HOOK_IT(17, CopyResource);
    HOOK_IT(25, SetPipelineState);
    HOOK_IT(47, ClearDepthStencilView);
    HOOK_IT(48, ClearRenderTargetView);
    HOOK_IT(59, ExecuteIndirect);
    gl->Release();
    Log("GpuTrace: command list hooks installed");
}

void GpuTraceRecordSetRT() { Record(M_SetRT); }

void GpuTraceRecordBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    if (g_state != 2) return;
    for (UINT i = 0; i < n; ++i) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !IsWatched(b[i].Transition.pResource)) continue;
        Log("GT #%d BACKBUFFER barrier %p: 0x%x -> 0x%x site=%llx", g_index, (void*)b[i].Transition.pResource,
            b[i].Transition.StateBefore, b[i].Transition.StateAfter, (unsigned long long)CallSite());
    }
}

void GpuTraceSetWatched(ID3D12Resource* const* res, int n)
{
    g_watchedCount = n > 8 ? 8 : n;
    for (int i = 0; i < g_watchedCount; ++i) g_watched[i] = res[i];
}

void GpuTraceMarker(const char* what)
{
    if (g_state == 2) {
        Log("GT #%d ---- %s ----", g_index, what);
        if (strstr(what, "end")) g_verbose = true;
    }
}

bool GpuTraceActive() { return g_state == 2; }

void GpuTraceArm()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state == 0) g_state = 1;
}

void GpuTraceOnPresent(bool begin)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (begin && g_state == 2) {
        g_state = 3;
        char line[512];
        int len = snprintf(line, sizeof(line), "GT summary: %d commands, %zu call sites |", g_index, g_sites.size());
        for (int i = 0; i < M_Count; ++i)
            len += snprintf(line + len, sizeof(line) - len, " %s=%d", kNames[i], g_counts[i]);
        Log("%s", line);
    } else if (!begin && g_state == 1) {
        g_state = 2;
        Log("GT: tracing one frame");
    }
}
