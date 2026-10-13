#pragma once
#include <d3d12.h>

// Debug: trace every D3D12 command recorded by the game during one frame.
void GpuTraceInstall(ID3D12CommandList* anyList);  // hooks the command list vtable
void GpuTraceArm();                                 // trace the next complete frame
void GpuTraceOnPresent(bool begin);                 // call at the start and the end of each Present
void GpuTraceSetWatched(ID3D12Resource* const* res, int n); // swapchain buffers to report
void GpuTraceMarker(const char* what);
bool GpuTraceActive();
void GpuTraceRecordSetRT();
void GpuTraceRecordBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b);
