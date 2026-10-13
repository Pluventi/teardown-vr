#pragma once

// Hardware data breakpoint on the calling thread (logs every instruction that
// reads/writes the address). len = 1, 2, 4 or 8 bytes.
void WatchMemory(void* addr, int len);
void StopWatch();

#include <d3d12.h>
// Call every Present: logs GPU status + main thread stack if the game freezes.
void HangDetectorTick(ID3D12Device* device);
