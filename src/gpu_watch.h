#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Watches the other programs that use the graphics card (the per-process
// numbers Task Manager shows). A machine-learning job or a second game can take
// most of the GPU and of its memory: the VR frame rate then collapses and the
// player has no way to know why. A background thread samples every 2 s.
namespace GpuWatch {

struct Hog
{
    std::string name;        // executable name (UTF-8), e.g. "python.exe"
    uint32_t    pid = 0;
    float       vramGB = 0;  // video memory resident for the process ("Local Usage")
    float       gpuPct = 0;  // busiest 3D / compute engine, percent of its time
};

struct Status
{
    bool available = false;  // the GPU performance counters can be read
    std::vector<Hog> hogs;   // programs competing with the game, worst first (at most 3)
    uint32_t serial = 0;     // incremented each time a new program starts competing
};

// Starts the background thread (only the first call does anything).
// adapterLuid: the game's GPU ((HighPart << 32) | LowPart); only that adapter is
// counted. 0 = every adapter.
void Start(uint64_t adapterLuid);

// Copies the latest result into `out` (the render thread calls this once a
// second). Never waits: if the watch thread is publishing at that instant it
// returns false and leaves `out` unchanged, so the lowest-priority thread can
// never stall a frame while it holds the lock.
bool Get(Status& out);

} // namespace GpuWatch
