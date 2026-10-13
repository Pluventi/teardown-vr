// Reverse-engineering helpers (debug builds of the investigation only).
//
// WatchMemory() puts a hardware data breakpoint (CPU debug register DR0) on an
// address in the game's main thread and logs every distinct instruction that
// touches it, with a short unwound call stack. Used to find which engine code
// reads the camera transform.
#include "re_tools.h"
#include "log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <cstdio>
#include <mutex>
#include <set>

namespace {

PVOID g_veh = nullptr;
std::mutex g_mutex;
std::set<DWORD64> g_seen;
int g_hits = 0;
DWORD g_thread = 0;

DWORD64 ModBase() { return (DWORD64)GetModuleHandleW(nullptr); }

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    DWORD64 base = ModBase();
    DWORD64 rip = c->Rip;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_hits;
        if (g_seen.size() < 64 && g_seen.insert(rip).second) {
            // Unwind a few frames from the faulting context.
            CONTEXT ctx = *c;
            char line[512];
            int len = snprintf(line, sizeof(line), "Watch hit rip=+%llx stack:", (unsigned long long)(rip - base));
            for (int i = 0; i < 10 && len < (int)sizeof(line) - 16; ++i) {
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!fn) break;
                PVOID handlerData;
                DWORD64 establisher;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                if (!ctx.Rip) break;
                if (imageBase == base)
                    len += snprintf(line + len, sizeof(line) - len, " %llx", (unsigned long long)(ctx.Rip - base));
                else
                    len += snprintf(line + len, sizeof(line) - len, " ?");
            }
            Log("%s", line);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct Arm { HANDLE thread; DWORD64 addr; int len; bool enable; };

DWORD WINAPI ArmThread(LPVOID p)
{
    Arm* a = (Arm*)p;
    SuspendThread(a->thread);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(a->thread, &c);
    if (a->enable) {
        c.Dr0 = a->addr;
        DWORD64 lenBits = a->len == 8 ? 2 : a->len == 4 ? 3 : a->len == 2 ? 1 : 0;
        // L0 enable, RW0 = 11 (read/write), LEN0
        c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (3ull << 16) | (lenBits << 18);
    } else {
        c.Dr0 = 0;
        c.Dr7 &= ~0xF0003ull;
    }
    SetThreadContext(a->thread, &c);
    ResumeThread(a->thread);
    return 0;
}

void SetBreakpoint(DWORD64 addr, int len, bool enable)
{
    // Every thread of the game (the engine runs jobs on worker threads).
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int count = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!t) continue;
        Arm a = { t, addr, len, enable };
        if (te.th32ThreadID == self) {
            HANDLE helper = CreateThread(nullptr, 0, ArmThread, &a, 0, nullptr);
            WaitForSingleObject(helper, 2000);
            CloseHandle(helper);
        } else {
            ArmThread(&a);
        }
        CloseHandle(t);
        ++count;
    }
    CloseHandle(snap);
    Log("Watch: %s on %d threads", enable ? "armed" : "disarmed", count);
}

} // namespace

void WatchMemory(void* addr, int len)
{
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
    g_thread = GetCurrentThreadId();
    Log("Watch: hardware breakpoint on %p (%d bytes), thread %lu", addr, len, g_thread);
    SetBreakpoint((DWORD64)addr, len, true);
}

void StopWatch()
{
    if (!g_thread) return;
    SetBreakpoint(0, 0, false);
    Log("Watch: stopped after %d hits, %zu distinct instructions", g_hits, g_seen.size());
}

// ---------------------------------------------------------------------------
// Hang detector: if the game stops presenting for 3 seconds, log the GPU
// status and the unwound call stack of the main (presenting) thread.
// ---------------------------------------------------------------------------
namespace {
volatile LONG64 g_lastPresentMs = 0;
DWORD g_presentThread = 0;
ID3D12Device* g_watchDevice = nullptr;

void LogThreadStack(DWORD tid)
{
    HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    if (!t) return;
    SuspendThread(t);
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    // (built while the thread is suspended, logged once it runs again: suspended inside Log, it
    // could hold the log's lock)
    char line[1024];
    line[0] = 0;
    if (GetThreadContext(t, &ctx)) {
        DWORD64 base = ModBase();
        int len = snprintf(line, sizeof(line), "Hang: main thread stack:");
        for (int i = 0; i < 24 && len < (int)sizeof(line) - 40; ++i) {
            HMODULE mod = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ctx.Rip, &mod);
            wchar_t name[MAX_PATH] = L"?";
            if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
            const wchar_t* shortName = wcsrchr(name, L'\\');
            shortName = shortName ? shortName + 1 : name;
            len += snprintf(line + len, sizeof(line) - len, " %ls+%llx", shortName,
                            (unsigned long long)(ctx.Rip - (DWORD64)mod));
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn) {
                // Leaf function: return address is at [rsp].
                ctx.Rip = *(DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
            } else {
                PVOID hd;
                DWORD64 est;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
            }
            if (!ctx.Rip) break;
        }
        (void)base;
    }
    ResumeThread(t);
    CloseHandle(t);
    if (line[0]) Log("%s", line);
}

DWORD WINAPI WatchdogThread(LPVOID)
{
    bool reported = false;
    for (;;) {
        Sleep(1000);
        LONG64 last = g_lastPresentMs;
        if (!last) continue;
        LONG64 now = (LONG64)GetTickCount64();
        if (now - last > 3000) {
            if (!reported) {
                reported = true;
                HRESULT reason = g_watchDevice ? g_watchDevice->GetDeviceRemovedReason() : S_OK;
                Log("Hang: no Present for %lld ms, device removed reason 0x%08x", now - last, (unsigned)reason);
                LogThreadStack(g_presentThread);
                LogFlush();   // (written now: the game may never recover)
            }
        } else {
            reported = false;
        }
    }
}
} // namespace

void HangDetectorTick(ID3D12Device* device)
{
    static bool started = false;
    g_watchDevice = device;
    g_presentThread = GetCurrentThreadId();
    g_lastPresentMs = (LONG64)GetTickCount64();
    if (!started) {
        started = true;
        CloseHandle(CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr));
    }
}
