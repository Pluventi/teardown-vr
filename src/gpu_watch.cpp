// Detects other programs that compete with the game for the graphics card.
//
// Windows publishes per-process GPU statistics as performance counters (the
// numbers Task Manager shows):
//   \GPU Process Memory(pid_<pid>_luid_0x<hi>_0x<lo>_phys_<n>)\Local Usage   bytes
//   \GPU Engine(pid_<pid>_luid_0x<hi>_0x<lo>_phys_<n>_eng_<e>_engtype_<type>)\Utilization Percentage
// "Local Usage" is the video memory really resident for the process. Not
// "Dedicated Usage": it also counts surfaces shared with other programs
// (measured on a high-end card: dwm.exe 17.5 GB and NVIDIA Overlay.exe 11.1 GB
// "dedicated" while the whole card held 2.3 GB; local usage 0.21 / 0.03 GB).
// They are read through PDH with their English names (PdhAddEnglishCounterW),
// so this works on a French Windows too. "Utilization Percentage" is a rate
// counter: each value is the average between two PdhCollectQueryData calls,
// here 2 s apart, on a low-priority background thread.
#include "gpu_watch.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <unordered_map>

namespace GpuWatch {
namespace {

constexpr DWORD  kPeriodMs   = 2000;
constexpr double kGB         = 1024.0 * 1024.0 * 1024.0;
constexpr double kVramLimit  = 3.0 * kGB;   // video memory resident for the process
constexpr double kBusyLimit  = 20.0;        // percent of the busiest 3D / compute engine
constexpr int    kOnSamples  = 2;           // over a limit 2 samples in a row (~4 s) before warning
constexpr int    kOffSamples = 3;           // under both limits 3 samples in a row (~6 s) before clearing
constexpr size_t kMaxHogs    = 3;

// Part of the VR chain or of Windows itself: never reported (prefix, any case).
const wchar_t* const kIgnoredPrefixes[] = {
    L"dwm.exe",           // Windows desktop compositor
    L"VirtualDesktop.",   // Virtual Desktop streamer / service / server
    L"OVR",               // Meta Quest Link (OVRServer_x64.exe, ...)
    L"vrserver.exe",      // SteamVR
    L"vrcompositor.exe",
};

std::mutex g_mutex;
Status g_state;        // guarded by g_mutex
uint64_t g_luid = 0;   // the game's adapter, written before the thread starts

struct Usage
{
    double vram = 0;   // bytes, summed over the process's allocations on the adapter
    double busy = 0;   // percent, busiest competing engine
};

struct Tracked
{
    std::wstring name;
    bool ignored = false;
    bool flagged = false;   // reported to the player
    int over = 0, under = 0;
    double vram = 0, busy = 0;
};

std::string Utf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Executable name of a process ("python.exe").
std::wstring ExeName(DWORD pid)
{
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            name = base ? base + 1 : path;
        }
        CloseHandle(h);
    }
    if (name.empty()) {
        // Elevated / protected processes: the process list still has the name.
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
                if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; }
            CloseHandle(snap);
        }
    }
    if (name.empty()) name = L"pid " + std::to_wstring(pid);
    return name;
}

bool Ignored(const std::wstring& name)
{
    for (const wchar_t* p : kIgnoredPrefixes)
        if (_wcsnicmp(name.c_str(), p, wcslen(p)) == 0) return true;
    return false;
}

// "pid_1234_luid_0x00000000_0x0000D1A5_phys_0..." -> pid, adapter LUID.
bool ParseInstance(const wchar_t* s, DWORD& pid, uint64_t& luid)
{
    unsigned long p = 0, hi = 0, lo = 0;
    if (!s || swscanf(s, L"pid_%lu_luid_0x%lx_0x%lx", &p, &hi, &lo) != 3) return false;
    pid = p;
    luid = (uint64_t(hi) << 32) | lo;
    return true;
}

// The engines the game needs: 3D (games), Compute_* / Cuda (machine learning,
// mining), Graphics_*. Copy and video encode / decode units are separate
// hardware and do not slow the game down.
bool CompetingEngine(const wchar_t* s)
{
    const wchar_t* t = wcsstr(s, L"_engtype_");
    if (!t) return false;
    t += 9;
    return _wcsnicmp(t, L"3D", 2) == 0 || _wcsnicmp(t, L"Compute", 7) == 0 || _wcsnicmp(t, L"Cuda", 4) == 0 ||
           _wcsnicmp(t, L"Graphics", 8) == 0;
}

bool Valid(const PDH_FMT_COUNTERVALUE& v)
{
    return v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA;
}

// All instances of a wildcard counter. buf is reused between samples.
bool ReadArray(PDH_HCOUNTER c, DWORD fmt, std::vector<BYTE>& buf, PDH_FMT_COUNTERVALUE_ITEM_W*& items, DWORD& count)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD bytes = (DWORD)buf.size();
        count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(c, fmt, &bytes, &count,
                                                     buf.empty() ? nullptr : (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data());
        if (st == ERROR_SUCCESS) {
            items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
            return true;
        }
        if (st != PDH_MORE_DATA) return false;
        buf.resize(bytes + 16384);   // processes may appear before the next call
    }
    return false;
}

// The previous list is swapped into `hogs` and freed by the caller, outside the lock.
void Publish(std::vector<Hog>&& hogs, uint32_t serial)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.hogs.swap(hogs);
    g_state.serial = serial;
}

DWORD WINAPI WatchThread(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER mem = nullptr, eng = nullptr;
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &query);
    if (st == ERROR_SUCCESS)
        st = PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Local Usage", 0, &mem);
    if (st == ERROR_SUCCESS)
        st = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &eng);
    if (st != ERROR_SUCCESS) {
        Log("GpuWatch: GPU performance counters unavailable (PDH 0x%08lx), other programs are not watched",
            (unsigned long)st);
        if (query) PdhCloseQuery(query);
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state.available = true;
    }
    Log("GpuWatch: watching other programs on the GPU (adapter luid 0x%llx, every %lu ms)",
        (unsigned long long)g_luid, (unsigned long)kPeriodMs);

    const DWORD self = GetCurrentProcessId();
    std::vector<BYTE> memBuf, engBuf;
    std::unordered_map<DWORD, Usage> usage;
    std::unordered_map<DWORD, Tracked> tracked;
    uint32_t serial = 0;
    bool primed = false;   // rate counters need a previous sample
    int slowLogs = 0, luidMisses = 0;
    for (;; Sleep(kPeriodMs)) {
        if (!Cfg().gpuWatch) {
            if (primed || !tracked.empty()) Publish({}, serial);
            tracked.clear();
            primed = false;
            continue;
        }
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) continue;
        QueryPerformanceCounter(&t1);
        double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        if (ms > 100.0 && slowLogs < 5) {
            ++slowLogs;
            Log("GpuWatch: reading the GPU counters took %.0f ms (background thread)", ms);
        }
        if (!primed) { primed = true; continue; }

        // Per process, on the game's adapter only.
        usage.clear();
        PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
        DWORD n = 0;
        DWORD pid;
        uint64_t luid;
        bool luidSeen = false;
        bool memOk = ReadArray(mem, PDH_FMT_LARGE, memBuf, items, n);
        if (memOk) {
            for (DWORD i = 0; i < n; ++i) {
                if (!Valid(items[i].FmtValue) || !ParseInstance(items[i].szName, pid, luid)) continue;
                if (luid == g_luid) luidSeen = true;
                if (g_luid && luid != g_luid) continue;
                usage[pid].vram += (double)items[i].FmtValue.largeValue;
            }
        }
        // The game's adapter missing from the counters (e.g. a new LUID after a
        // driver reset): count every adapter rather than see nothing.
        if (memOk) luidMisses = luidSeen ? 0 : luidMisses + 1;
        if (g_luid && luidMisses >= 3) {
            Log("GpuWatch: adapter luid 0x%llx not in the GPU counters, counting every adapter",
                (unsigned long long)g_luid);
            g_luid = 0;
        }
        if (ReadArray(eng, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, engBuf, items, n)) {
            for (DWORD i = 0; i < n; ++i) {
                if (!Valid(items[i].FmtValue) || !CompetingEngine(items[i].szName)) continue;
                if (!ParseInstance(items[i].szName, pid, luid) || (g_luid && luid != g_luid)) continue;
                Usage& u = usage[pid];
                u.busy = std::max(u.busy, items[i].FmtValue.doubleValue);   // like Task Manager: busiest engine
            }
        }

        // Programs over a limit this sample (names resolved once, only for them).
        for (auto& [p, u] : usage) {
            if (p == self || p == 0 || (u.vram <= kVramLimit && u.busy <= kBusyLimit)) continue;
            Tracked& t = tracked[p];
            if (t.name.empty()) {
                t.name = ExeName(p);
                t.ignored = Ignored(t.name);
            }
        }

        // Debounce: report after kOnSamples samples over a limit, clear after
        // kOffSamples under both (at once when the program has exited).
        std::vector<Hog> hogs;
        for (auto it = tracked.begin(); it != tracked.end();) {
            Tracked& t = it->second;
            auto u = usage.find(it->first);
            bool present = u != usage.end();
            bool over = present && (u->second.vram > kVramLimit || u->second.busy > kBusyLimit);
            t.vram = present ? u->second.vram : 0;
            t.busy = present ? u->second.busy : 0;
            if (over) {
                t.under = 0;
                ++t.over;
            } else {
                t.over = 0;
                t.under = present ? t.under + 1 : kOffSamples;
            }
            std::string name = Utf8(t.name);
            if (!t.ignored && !t.flagged && t.over >= kOnSamples) {
                t.flagged = true;
                ++serial;
                Log("GpuWatch: %s (pid %lu) is using the GPU too: %.1f GB video memory, %.0f%% engine time "
                    "- the game may slow down", name.c_str(), (unsigned long)it->first, t.vram / kGB, t.busy);
            }
            if (!over && (!t.flagged || t.under >= kOffSamples)) {
                if (t.flagged) Log("GpuWatch: %s (pid %lu) no longer competes for the GPU", name.c_str(),
                                   (unsigned long)it->first);
                it = tracked.erase(it);
                continue;
            }
            if (t.flagged) hogs.push_back({ name, (uint32_t)it->first, float(t.vram / kGB), float(t.busy) });
            ++it;
        }
        std::sort(hogs.begin(), hogs.end(), [](const Hog& a, const Hog& b) {
            return a.gpuPct / kBusyLimit + a.vramGB * kGB / kVramLimit > b.gpuPct / kBusyLimit + b.vramGB * kGB / kVramLimit;
        });
        if (hogs.size() > kMaxHogs) hogs.resize(kMaxHogs);
        Publish(std::move(hogs), serial);
    }
}

} // namespace

void Start(uint64_t adapterLuid)
{
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) return;
    g_luid = adapterLuid;
    // A plain Win32 thread that is never joined: a std::thread object still
    // running when the DLL's globals are destroyed at exit calls std::terminate.
    if (HANDLE h = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr)) CloseHandle(h);
    else Log("GpuWatch: thread creation failed");
}

bool Get(Status& out)
{
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    out = g_state;
    return true;
}

} // namespace GpuWatch
