#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

// Small D3D12 toolkit living on the game's device / queue: command lists,
// descriptor heaps and a full-screen "blit" shader used to copy the game image
// into the headset swapchains (with colour conversion, sharpening, cropping).

struct BlitParams
{
    float srcRect[4] = { 0, 0, 1, 1 };   // uv offset (xy) + uv scale (zw)
    float linearize = 0;                 // 1 = convert sRGB -> linear in shader
    float sharpen = 0;
    float brightness = 1;
    float opaque = 1;                    // 1 = force alpha to 1
    float texel[2] = { 0, 0 };           // 1 / source size
    float vignette = 0;                  // comfort vignette strength (0..1)
    float pad = 0;
};

enum class Blend { None, Premultiplied };

namespace Render {

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);
bool Ready();
ID3D12Device* Device();
ID3D12CommandQueue* Queue();

// Per-present command list. Begin() waits for the GPU if the ring is full.
ID3D12GraphicsCommandList* Begin();
int FrameSlot();              // ring slot used by the current command list (0..3)
void Submit();                 // close + execute on the game queue + signal
void WaitIdle();

// Transient SRV for a resource (valid for the current frame only).
D3D12_GPU_DESCRIPTOR_HANDLE TransientSrv(ID3D12Resource* res, DXGI_FORMAT fmt);
// Persistent descriptors.
D3D12_CPU_DESCRIPTOR_HANDLE AllocRtv();
void AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
ID3D12DescriptorHeap* SrvHeap();
D3D12_CPU_DESCRIPTOR_HANDLE TransientRtv(ID3D12Resource* res, DXGI_FORMAT fmt);

void Blit(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE src, D3D12_CPU_DESCRIPTOR_HANDLE dst,
          DXGI_FORMAT dstFormat, UINT dstX, UINT dstY, UINT dstW, UINT dstH, const BlitParams& p, Blend blend = Blend::None);

void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

bool IsSrgb(DXGI_FORMAT f);
DXGI_FORMAT ToSrgb(DXGI_FORMAT f);
DXGI_FORMAT ToLinear(DXGI_FORMAT f);
DXGI_FORMAT TypelessToTyped(DXGI_FORMAT f);

} // namespace Render
