#pragma once

// Diagnostics ([Debug] debugProfile=1): samples the calling thread's call stack
// about 1000 times per second. Call SamplerStart from the game's main thread.
void SamplerStart();
// Logs where the main thread spent the time between t0 and t1 (ProfNow() ms).
void SamplerReport(double t0, double t1);
