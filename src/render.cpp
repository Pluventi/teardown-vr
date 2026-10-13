#include "render.h"
#include "log.h"

#include <d3dcompiler.h>
#include <map>
#include <cstring>

namespace Render {
namespace {

constexpr int kFrames = 4;
constexpr int kSrvPersistent = 64;
constexpr int kSrvTransientPerFrame = 32;
constexpr int kSrvTotal = kSrvPersistent + kFrames * kSrvTransientPerFrame;
constexpr int kRtvPersistent = 192;
constexpr int kRtvTransient = 64;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;

ID3D12CommandAllocator* g_alloc[kFrames] = {};
UINT64 g_allocFence[kFrames] = {};
int g_frame = 0;
ID3D12GraphicsCommandList* g_cl = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0;
HANDLE g_event = nullptr;

ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
UINT g_srvInc = 0, g_rtvInc = 0;
int g_srvNext = 0, g_rtvNext = 0;
int g_srvTransient = 0, g_rtvTransient = 0;

ID3D12RootSignature* g_rootSig = nullptr;
ID3DBlob* g_vs = nullptr;
ID3DBlob* g_ps = nullptr;
std::map<std::pair<int, int>, ID3D12PipelineState*> g_psos;
bool g_ready = false;

const char* kShader = R"(
cbuffer Params : register(b0)
{
    float4 srcRect;
    float  linearize;
    float  sharpen;
    float  brightness;
    float  opaque;
    float2 texel;
    float  vignette;
    float  pad;
};
Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 nuv : TEXCOORD1; };

VSOut VSMain(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = srcRect.xy + uv * srcRect.zw;
    o.nuv = uv;
    return o;
}

float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return float3(c.x <= 0.04045 ? lo.x : hi.x, c.y <= 0.04045 ? lo.y : hi.y, c.z <= 0.04045 ? lo.z : hi.z);
}

float4 PSMain(VSOut i) : SV_Target
{
    if (i.uv.x < 0 || i.uv.y < 0 || i.uv.x > 1 || i.uv.y > 1)
        return float4(0, 0, 0, opaque > 0.5 ? 1 : 0);
    float4 c = tex.SampleLevel(smp, i.uv, 0);
    if (sharpen > 0)
    {
        float3 n = tex.SampleLevel(smp, i.uv + float2(texel.x, 0), 0).rgb
                 + tex.SampleLevel(smp, i.uv - float2(texel.x, 0), 0).rgb
                 + tex.SampleLevel(smp, i.uv + float2(0, texel.y), 0).rgb
                 + tex.SampleLevel(smp, i.uv - float2(0, texel.y), 0).rgb;
        c.rgb = saturate(c.rgb + (c.rgb - n * 0.25) * sharpen);
    }
    c.rgb *= brightness;
    if (vignette > 0)
    {
        float2 d = i.nuv * 2 - 1;
        float r = length(d);
        float inner = lerp(1.2, 0.35, vignette);
        c.rgb *= 1 - smoothstep(inner, inner + 0.35, r);
    }
    if (linearize > 0.5)
        c.rgb = SrgbToLinear(saturate(c.rgb));
    if (opaque > 0.5)
        c.a = 1;
    return c;
}
)";

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);

bool CompileShaders()
{
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) { Log("Render: d3dcompiler_47.dll missing"); return false; }
    auto compile = (PFN_D3DCompile)GetProcAddress(m, "D3DCompile");
    if (!compile) return false;
    ID3DBlob* err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "blit", nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_vs, &err))) {
        Log("Render: VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(compile(kShader, strlen(kShader), "blit", nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_ps, &err))) {
        Log("Render: PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    return true;
}

bool CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(BlitParams) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &samp;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        Log("Render: root signature serialize failed");
        return false;
    }
    HRESULT hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
    blob->Release();
    return SUCCEEDED(hr);
}

ID3D12PipelineState* GetPso(DXGI_FORMAT fmt, Blend blend)
{
    auto key = std::make_pair((int)fmt, (int)blend);
    auto it = g_psos.find(key);
    if (it != g_psos.end()) return it->second;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_rootSig;
    d.VS = { g_vs->GetBufferPointer(), g_vs->GetBufferSize() };
    d.PS = { g_ps->GetBufferPointer(), g_ps->GetBufferSize() };
    auto& rt = d.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (blend == Blend::Premultiplied) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = fmt;
    d.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        Log("Render: PSO creation failed for format %d (0x%08x)", fmt, hr);
        pso = nullptr;
    }
    g_psos[key] = pso;
    return pso;
}

} // namespace

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    g_device = device;
    g_queue = queue;

    for (int i = 0; i < kFrames; ++i)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i])))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_cl)))) return false;
    g_cl->Close();
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvTotal;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kRtvPersistent + kRtvTransient;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
    g_srvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!CompileShaders() || !CreateRootSignature()) return false;
    g_ready = true;
    Log("Render: initialised");
    return true;
}

bool Ready() { return g_ready; }
int FrameSlot() { return g_frame; }
ID3D12Device* Device() { return g_device; }
ID3D12CommandQueue* Queue() { return g_queue; }
ID3D12DescriptorHeap* SrvHeap() { return g_srvHeap; }

ID3D12GraphicsCommandList* Begin()
{
    g_frame = (g_frame + 1) % kFrames;
    if (g_fence->GetCompletedValue() < g_allocFence[g_frame]) {
        g_fence->SetEventOnCompletion(g_allocFence[g_frame], g_event);
        WaitForSingleObject(g_event, 1000);
    }
    g_alloc[g_frame]->Reset();
    g_cl->Reset(g_alloc[g_frame], nullptr);
    g_srvTransient = 0;
    g_rtvTransient = 0;
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
    g_cl->SetDescriptorHeaps(1, heaps);
    return g_cl;
}

void Submit()
{
    g_cl->Close();
    ID3D12CommandList* lists[] = { g_cl };
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_allocFence[g_frame] = g_fenceValue;
}

void WaitIdle()
{
    if (!g_fence) return;
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_fence->SetEventOnCompletion(g_fenceValue, g_event);
    WaitForSingleObject(g_event, 2000);
}

D3D12_GPU_DESCRIPTOR_HANDLE TransientSrv(ID3D12Resource* res, DXGI_FORMAT fmt)
{
    int slot = kSrvPersistent + g_frame * kSrvTransientPerFrame + (g_srvTransient++ % kSrvTransientPerFrame);
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * g_srvInc;
    g_device->CreateShaderResourceView(res, &d, cpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += (UINT64)slot * g_srvInc;
    return gpu;
}

D3D12_CPU_DESCRIPTOR_HANDLE TransientRtv(ID3D12Resource* res, DXGI_FORMAT fmt)
{
    int slot = kRtvPersistent + (g_rtvTransient++ % kRtvTransient);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * g_rtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC d = {};
    d.Format = fmt;
    d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(res, &d, cpu);
    return cpu;
}

D3D12_CPU_DESCRIPTOR_HANDLE AllocRtv()
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(g_rtvNext++ % kRtvPersistent) * g_rtvInc;
    return cpu;
}

void AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    int slot = g_srvNext++ % kSrvPersistent;
    cpu->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * g_srvInc;
    gpu->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)slot * g_srvInc;
}

void Blit(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE src, D3D12_CPU_DESCRIPTOR_HANDLE dst,
          DXGI_FORMAT dstFormat, UINT x, UINT y, UINT w, UINT h, const BlitParams& p, Blend blend)
{
    ID3D12PipelineState* pso = GetPso(dstFormat, blend);
    if (!pso) return;
    cl->SetPipelineState(pso);
    cl->SetGraphicsRootSignature(g_rootSig);
    cl->SetGraphicsRoot32BitConstants(0, sizeof(BlitParams) / 4, &p, 0);
    cl->SetGraphicsRootDescriptorTable(1, src);
    D3D12_VIEWPORT vp = { (float)x, (float)y, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &dst, FALSE, nullptr);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
}

void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

bool IsSrgb(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

DXGI_FORMAT ToSrgb(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return f;
    }
}

DXGI_FORMAT ToLinear(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

DXGI_FORMAT TypelessToTyped(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

} // namespace Render
