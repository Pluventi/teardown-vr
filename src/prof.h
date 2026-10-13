#pragma once
#include <windows.h>

// Where the time of one engine frame goes (milliseconds, main thread only).
// Accumulated between two Presents and read by the frame statistics in vr.cpp,
// which logs the breakdown of frames that took much longer than usual.
// The first five sections are always timed and do not overlap. The others are only timed
// with [Debug] perfTrace (perf.h):
//   P_Ours, P_Particles, P_FixedLoop: parts of what the five leave out ("physics/other"):
//     our code at render entry (engine.cpp Hook_Render: per-eye history, TAA, FixObjectMotion,
//     OffArmLateFix), ParticleSystem::update, and the time from Game::update's exit to
//     ParticleSystem::update's entry (the fixed-step physics loop and the start of the draw);
//   P_JobWait, P_GpuWait, P_ShapeWait, P_DisplayWait: waits, which overlap the others (main thread
//     blocked in Job::wait, in TFenceD3D12::waitValueOnCpu, in the shape interpolation wait of
//     FixObjectMotion, on the desktop swap chain's frame-latency object after a Present - "of which").
enum ProfSection
{
    P_Update, P_Render, P_OurPresent, P_RealPresent, P_PostPresent,
    P_Ours, P_Particles, P_FixedLoop,
    P_JobWait, P_GpuWait, P_ShapeWait, P_DisplayWait,
    P_Count
};

inline double g_prof[P_Count] = {};

// [Debug] perfTrace (0 = off: nothing above P_PostPresent is timed and nothing is hooked for it;
// 1 = per-eye sections, engine profiler, waits, per-thread CPU; 2 = also each mod's tick, the
// Lua binding census and per-binding times). Set once after the ini is read (main.cpp).
inline int g_perfTrace = 0;

inline double ProfNow()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// The sections that were always timed (their sum leaves "physics/other").
inline double ProfKnownBase()
{
    return g_prof[P_Update] + g_prof[P_Render] + g_prof[P_OurPresent] + g_prof[P_RealPresent] + g_prof[P_PostPresent];
}

struct ProfScope
{
    ProfSection s;
    double t0;
    explicit ProfScope(ProfSection sec) : s(sec), t0(ProfNow()) {}
    ~ProfScope() { g_prof[s] += ProfNow() - t0; }
};
