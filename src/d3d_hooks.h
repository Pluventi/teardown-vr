#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>

// Hooks DXGI so we can see the game's swapchain, its command queue and every
// Present call.
void D3DHooksInit();
// Before the game reads its options: a "full screen" display mode is switched to
// "borderless window" (see d3d_hooks.cpp).
void FixFullscreenOption();

// Debug: log where the game submits GPU work from (set once the Lua mod is active).
extern bool g_traceGpuSubmits;

// Implemented by the frame orchestrator (vr.cpp).
void OnGamePresent(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, UINT& syncInterval, UINT& flags);
void OnGameBeforeResize(IDXGISwapChain3* swapchain);
void OnGameAfterResize(IDXGISwapChain3* swapchain);
void OnGamePostPresent();   // after the real Present returned (headset pacing)

// Optional override of the swapchain size (0 = keep the game's choice).
void GetSwapchainSizeOverride(UINT& width, UINT& height);
