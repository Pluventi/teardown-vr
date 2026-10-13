// Diagnostics: a sampling profiler for the game's main thread ([Debug]
// debugProfile=1). About 1000 times per second the main thread is briefly
// suspended and its call stack recorded; for a frame that took too long, the
// functions it spent its time in are written to the log. Only for finding
// stutter: it slows the game down slightly.
#include "sampler.h"
#include "log.h"
#include "prof.h"

#pragma comment(lib, "winmm.lib")

#include <windows.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kDepth = 14;
constexpr int kRing = 1 << 14;

struct Sample
{
    double t;
    DWORD64 pc[kDepth];
    int n;
};

Sample g_ring[kRing];
std::atomic<uint32_t> g_head{ 0 };
HANDLE g_main = nullptr;
std::atomic<bool> g_run{ false };
BYTE* g_exeBase = nullptr;
size_t g_exeSize = 0;

void Run()
{
    timeBeginPeriod(1);
    while (g_run) {
        Sleep(1);
        if (SuspendThread(g_main) == (DWORD)-1) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        Sample s{};
        s.t = ProfNow();
        if (GetThreadContext(g_main, &ctx)) {
            // Walk the stack while the thread is stopped (no allocation here: the
            // stopped thread may hold the heap lock).
            for (int i = 0; i < kDepth && ctx.Rip; ++i) {
                s.pc[s.n++] = ctx.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!fn) {   // leaf function: return address on top of the stack
                    ctx.Rip = *(DWORD64*)ctx.Rsp;
                    ctx.Rsp += 8;
                    continue;
                }
                PVOID handler = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
            }
        }
        ResumeThread(g_main);
        uint32_t h = g_head.fetch_add(1);
        g_ring[h % kRing] = s;
    }
    timeEndPeriod(1);
}

std::string Where(DWORD64 pc)
{
    if (g_exeBase && pc >= (DWORD64)g_exeBase && pc < (DWORD64)g_exeBase + g_exeSize) {
        // Report the start of the function (pdata) so samples group by function.
        DWORD64 imageBase = 0;
        if (PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &imageBase, nullptr)) {
            char b[32];
            snprintf(b, sizeof(b), "exe+%x", fn->BeginAddress);
            return b;
        }
        char b[32];
        snprintf(b, sizeof(b), "exe@%llx", (unsigned long long)(pc - (DWORD64)g_exeBase));
        return b;
    }
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &m) && m) {
        char name[MAX_PATH];
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        char b[300];
        snprintf(b, sizeof(b), "%s+%llx", base, (unsigned long long)(pc - (DWORD64)m));
        return b;
    }
    return "?";
}

} // namespace

void SamplerStart()
{
    if (g_run) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) return;
    g_exeBase = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    g_exeSize = nt->OptionalHeader.SizeOfImage;
    g_run = true;
    std::thread(Run).detach();
    Log("Sampler: profiling the main thread (debugProfile=1)");
}

void SamplerReport(double t0, double t1)
{
    if (!g_run) return;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&SamplerReport, &self);
    auto inExe = [](DWORD64 pc) { return pc >= (DWORD64)g_exeBase && pc < (DWORD64)g_exeBase + g_exeSize; };
    auto modOf = [](DWORD64 pc) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &m);
        return m;
    };
    // Each sample goes to the mod (this DLL on the stack) or to the game; game
    // samples are grouped by their first three game functions.
    std::map<std::string, int> game, mod;
    std::map<std::string, std::string> example;
    int total = 0, ours = 0;
    uint32_t head = g_head.load();
    for (uint32_t i = 0; i < kRing && i < head; ++i) {
        const Sample& s = g_ring[(head - 1 - i) % kRing];
        if (s.t < t0) break;
        if (s.t > t1 || !s.n) continue;
        ++total;
        int selfAt = -1;
        for (int k = 0; k < s.n && selfAt < 0; ++k)
            if (modOf(s.pc[k]) == self) selfAt = k;
        std::string chain;
        for (int k = 0; k < s.n; ++k) chain += (k ? " < " : "") + Where(s.pc[k]);
        if (selfAt >= 0) {
            ++ours;
            std::string key = Where(s.pc[0]) + " in " + Where(s.pc[selfAt]);
            mod[key]++;
            example[key] = chain;
            continue;
        }
        std::string key;
        for (int k = 0, c = 0; k < s.n && c < 3; ++k)
            if (inExe(s.pc[k])) { key += (c ? " < " : "") + Where(s.pc[k]); ++c; }
        if (key.empty()) key = Where(s.pc[0]);
        game[key]++;
        example[key] = chain;
    }
    auto top = [&](const std::map<std::string, int>& m, int n) {
        std::vector<std::pair<int, std::string>> v;
        for (auto& kv : m) v.push_back({ kv.second, kv.first });
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && (int)i < n; ++i)
            Log("    [%d] %s   | %s", v[i].first, v[i].second.c_str(), example[v[i].second].c_str());
    };
    Log("  samples %d: mod/VR runtime %d, game %d", total, ours, total - ours);
    top(game, 4);
    top(mod, 2);
}