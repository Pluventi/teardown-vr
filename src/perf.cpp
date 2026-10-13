// Performance measurement and process priority (see perf.h).
#include "perf.h"
#include "config.h"
#include "frameb.h"
#include "hooks.h"
#include "log.h"
#include "prof.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <tlhelp32.h>
// (after windows.h, WIN32_LEAN_AND_MEAN: MIB_IF_TABLE2 for the network send rate; nothing linked,
// iphlpapi.dll is loaded by the sampler thread)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace Perf {
namespace {

using PFN_Any4 = __int64(__fastcall*)(void*, void*, void*, void*);

BYTE* g_base = nullptr;          // teardown.exe
size_t g_imageSize = 0;
std::atomic<DWORD> g_mainTid{ 0 };   // the game's main thread (Game::update), 0 before the first update
int g_updEye = 0;                // eye of the frame being updated / drawn: 0 = A (left), 1 = B (right)
uint64_t g_frameNo = 0;          // game updates seen

inline bool OnMain() { return GetCurrentThreadId() == g_mainTid.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------------------------------
// This frame (main thread), added to its eye at the Present (FrameEnd)
// ---------------------------------------------------------------------------------------------------
// Categories of the engine's profiler (Profiler::add; names from its overlay, "Profiler (F3 to hide)").
constexpr int kCats = 10;
const char* const kCatNames[kCats] = { "total", "destr", "part", "path", "phys", "rend", "screen", "scrS", "scr", "water" };
double g_catFrame[kCats] = {};
int g_stepsFrame = -1;           // physics steps of this frame (-1: not measured)
// rva 0: the callers past the table's size, together ("+other").
struct WaitBy { DWORD rva = 0; double ms = 0; int n = 0; };
constexpr int kWaitFrame = 24;
WaitBy g_waitFrame[kWaitFrame];  // main thread's Job::wait time, by calling function (teardown.exe RVA)
int g_waitFrameN = 0;
constexpr size_t kWaitEye = 24;  // callers kept per eye (EyeSums::waits; then "+other")


// ---------------------------------------------------------------------------------------------------
// [Performance] perfDetail (perfTrace >= 1): the "Perf waits / jobs / GPU" lines (LogInstrument)
// ---------------------------------------------------------------------------------------------------
// 1 = the main thread's blocking Job::wait calls, its GPU fence waits by caller and the async uploader's
// bytes; 2 = every job timed by class (Job::run, all threads); 4 = the GPU's busy and idle time per frame
// (timestamps). The hooks are installed for the bits asked for at start; the control file then switches
// those bits live, at a statistics line (each 600-frame window is measured one way).
constexpr int kDetailWaits = 1, kDetailJobs = 2, kDetailGpu = 4;
// 8 = the GPU time of each pass of the game's frame ("GPU time per pass" below; it implies 4: DetailWanted).
constexpr int kDetailPasses = 8;
int g_detailHooks = 0;                  // bits set up at start (their hooks found and installed)
std::atomic<int> g_detail{ 0 };         // bits on now (read by every thread)

// A blocking Job::wait: the job was not done when the main thread asked (state +8 != 4) and the call took
// more than 2 us (pooled event, ResetEvent, the extra worker's SetEvent, the kernel wait).
constexpr int kHist = 8;
const double kHistMs[kHist - 1] = { 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0 };
const char* const kHistName[kHist] = { "<20us", "<50us", "<0.1ms", "<0.2ms", "<0.5ms", "<1ms", "<2ms", "2ms+" };
// The job's state then: 0 not scheduled yet, 1 waiting for its dependencies, 2 queued (no worker has taken
// it: a wake-up away), 3 running.
const char* const kStateName[4] = { "new", "deps", "queued", "running" };
constexpr int kFenceBy = 6;             // GPU fence wait callers kept per frame and per eye (then "+other")
struct DetailFrame
{
    int blocking = 0;
    double blockingMs = 0;
    int hist[kHist] = {};
    int state[4] = {};
    WaitBy fence[kFenceBy];             // TFenceD3D12::waitValueOnCpu by calling function (waits over 2 us)
    int fenceN = 0;
};
DetailFrame g_detFrame;                 // this frame (main thread), added to its eye at FrameEnd
// Other threads' GPU fence waits and the async uploader's submissions: any thread (taken at FrameEnd).
std::atomic<uint64_t> g_offFenceUs{ 0 };
std::atomic<int> g_offFenceN{ 0 };
std::atomic<uint64_t> g_upBytes{ 0 };
std::atomic<int> g_upSubmits{ 0 }, g_upForced{ 0 };

// Adds to a table of callers of cap entries (the last one: the callers past it together, rva 0, "+other").
void AddBy(WaitBy* t, int& n, int cap, DWORD rva, double ms, int count)
{
    int i = 0;
    while (i < n && t[i].rva != rva) ++i;
    if (i == n) {
        if (n < cap - 1) {
            t[n++] = { rva, 0, 0 };
        } else {
            i = cap - 1;
            if (n < cap) t[n++] = { 0, 0, 0 };
        }
    }
    t[i].ms += ms;
    t[i].n += count;
}

void NoteJobWait(int state, double ms)
{
    if (!(g_detail.load(std::memory_order_relaxed) & kDetailWaits) || state < 0 || state > 3 || ms < 0.002) return;
    DetailFrame& f = g_detFrame;
    ++f.blocking;
    f.blockingMs += ms;
    int b = 0;
    while (b < kHist - 1 && ms >= kHistMs[b]) ++b;
    ++f.hist[b];
    ++f.state[state];
}

// Per eye: 0 = A, 1 = B, 2 = neither (menus, mono).
struct EyeSums
{
    int frames = 0;
    double frameMs = 0;
    double sec[P_Count] = {};
    double cat[kCats] = {};
    int stepFrames = 0;
    double steps = 0;
    std::vector<WaitBy> waits;
};
EyeSums g_eye[3];
int g_lastFrames[3] = {};        // frames of the last stats window (LogDetails' averages)
int g_shapesLast = 0;
double g_movedSum = 0;
int g_movedPairs = 0;

// ---------------------------------------------------------------------------------------------------
// perfTrace 2: mods, bindings
// ---------------------------------------------------------------------------------------------------
// Binding names -> slots (open addressing on the name pointer: the engine passes the same string
// of its .rdata for each binding at every call).
constexpr int kSlots = 1024;
const char* g_slotName[kSlots] = {};
bool g_slotRead[kSlots] = {};    // a binding that only reads (frame B mode 1: IsReadOnly)
int g_slotCount = 0;
struct BindSums { double ms = 0; unsigned calls = 0; };
BindSums g_bind[2][kSlots];

// Bindings that only read (the allowlist of the plan's Step 6, by name).
bool IsRead(const char* n)
{
    return strncmp(n, "Get", 3) == 0 || strncmp(n, "Query", 5) == 0 || strncmp(n, "Is", 2) == 0 ||
           strncmp(n, "Has", 3) == 0 || strncmp(n, "Find", 4) == 0;
}

// The same, for skipping a frame-B tick: QueryPath starts a path search (an AI's state), so it
// counts as a write. GetLastSound too: it returns the loudest sound of the frame before (moved
// there at each frame start, SoundManager::update 0xff911), so frame A's gunshots and explosions
// are only readable in frame B - a listener whose frame-B tick were skipped would never hear them.
bool IsReadOnly(const char* n)
{
    return IsRead(n) && strcmp(n, "QueryPath") != 0 && strcmp(n, "GetLastSound") != 0;
}

int SlotOf(const char* name)
{
    if (!name) return -1;
    size_t h = (size_t)(((uintptr_t)name >> 3) * 0x9E3779B97F4A7C15ull >> 54);   // 10 bits
    for (int i = 0; i < kSlots; ++i) {
        const size_t k = (h + i) & (kSlots - 1);
        if (g_slotName[k] == name) return (int)k;
        if (!g_slotName[k]) {
            if (g_slotCount >= kSlots * 3 / 4) return -1;
            g_slotName[k] = name;
            g_slotRead[k] = IsReadOnly(name);
            ++g_slotCount;
            return (int)k;
        }
    }
    return -1;
}

// A script entity (a mod's script: its server script at +0x48, its client script at +0x40).
constexpr uint32_t kEntClient = 0x40, kEntServer = 0x48;
uint32_t g_entPath = 0;   // its script path: the engine's string (32 bytes; byte +0x1f != 0: heap pointer at +0); 0 = unknown
struct ModRec
{
    BYTE* ent = nullptr;
    void* scripts[2] = {};
    char name[64] = "";
    double ms[2] = {};
    unsigned calls[2] = {};
    std::bitset<kSlots> used[2];     // bindings called in its ticks (A, B)
    std::bitset<kSlots> logged[2];   // ... already in the log
    uint64_t seen = 0;               // g_frameNo of its last tick
    // Frame B mode 1 (frameb.h)
    uint64_t aFrame = 0;             // g_frameNo of its last frame-A tick
    bool aClean = false;             // that tick only read: no need to tick it again in frame B
    double aMs = 0;                  // its time
    bool ours = false;               // our Lua mod (it sent a TDVR message): always ticked
    bool keep = false;               // [Performance] frameBKeepMods: always ticked
};
std::unordered_map<BYTE*, ModRec> g_mods;
ModRec* g_tickMod = nullptr;          // the mod whose tick runs now (main thread)
std::vector<std::string> g_keepMods;  // [Performance] frameBKeepMods, lower case

// Frame B mode 1 counters (since the last "Stereo frames" line)
struct SkipStats { int frames = 0, skipped = 0, ticks = 0; double ms = 0; } g_skip;
bool g_census = false;                // mods' tick and dispatcher hooked
bool g_skipReady = false;             // ... and the registry setters: mode 1 possible

// The script path (copied under SEH: a field read in an entity the engine owns).
bool ReadPath(const BYTE* ent, char* out, size_t n)
{
    out[0] = 0;
    if (!g_entPath) return false;
    __try {
        const BYTE* s = ent + g_entPath;
        const char* p = s[0x1f] ? *(const char* const*)s : (const char*)s;
        size_t i = 0;
        for (; i + 1 < n && p[i]; ++i) {
            if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7e) return false;
            out[i] = p[i];
        }
        out[i] = 0;
        return i > 0 && !p[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

// "...\mods\TeardownVR\main.lua" -> "TeardownVR/main.lua" (the last two parts of the path).
void ModName(BYTE* ent, char* name, size_t n)
{
    char path[300];
    if (!ReadPath(ent, path, sizeof(path))) {
        snprintf(name, n, "entity %p", (void*)ent);
        return;
    }
    const char* start = path;
    int seps = 0;
    for (const char* p = path + strlen(path); p > path; --p)
        if (p[-1] == '/' || p[-1] == '\\') {
            if (++seps == 2) { start = p; break; }
        }
    size_t len = strlen(start);
    if (len >= n) start += len - (n - 1);   // (the end: the folder and file names)
    snprintf(name, n, "%s", start);
    for (char* c = name; *c; ++c)
        if (*c == '\\') *c = '/';
}

ModRec* ModOf(BYTE* ent)
{
    void* client = *(void**)(ent + kEntClient);
    void* server = *(void**)(ent + kEntServer);
    ModRec& m = g_mods[ent];
    if (m.ent != ent || m.scripts[0] != client || m.scripts[1] != server) {
        // (a new entity, or another script at a recycled address: a new level)
        m = ModRec{};
        m.ent = ent;
        m.scripts[0] = client;
        m.scripts[1] = server;
        ModName(ent, m.name, sizeof(m.name));
        if (!g_keepMods.empty()) {
            std::string low = m.name;
            for (char& c : low) c = (char)tolower((unsigned char)c);
            for (const std::string& k : g_keepMods)
                if (low.find(k) != std::string::npos) m.keep = true;
            if (m.keep) Log("Perf: frame B: '%s' always ticked in frame B (frameBKeepMods)", m.name);
        }
    }
    return &m;
}

// ---------------------------------------------------------------------------------------------------
// Job-wait latency ([Performance] jobSpin, jobSpinUs, mainSpinUs; perf.h)
// ---------------------------------------------------------------------------------------------------
// The engine's job system puts its threads to sleep in the kernel at once (2.0.1 addresses). An idle worker takes a
// token (lock xadd [jm],-1 at +5021ec) and, when there was none (count <= 0 before), waits INFINITE on the kernel
// semaphore jm+100h (+5111d0, called there only). A signal (+501e30) raises the count with a CAS, then calls
// ReleaseSemaphore(min(n, workers, waiters)). The main thread's Job::wait (+5025e0) resets a pooled event, wakes the
// extra worker (SetEvent jm+150h) and waits INFINITE. Each fork thus pays the workers' wake-ups (ReleaseSemaphore
// readying up to 15 threads, then ~15-20 us each) and each join the main thread's. In frame A of the headset session
// only (not in frame B, the menus or the headset wait: power and heat):
//  - an idle worker spins up to jobSpinUs before it sleeps, napping with MWAITX on the count's cache line (AMD;
//    PAUSE otherwise); when the count rises it takes a unit with the engine's own wait function and a timeout of 0.
//    It only reads the count and the running flag, and the semaphore is only touched by the engine's function: the
//    spin is what happens when a worker is slow to reach its wait (pre-empted between its xadd and the call). No
//    wake is lost (a unit stays until a wait takes it), none is taken twice (WAIT_TIMEOUT changes nothing), and
//    without a unit in time the INFINITE wait follows as before. Never on the other logical processor of the main
//    thread's core while the main thread runs (it would slow it down); a spin that ended in sleep halves the next.
//  - the main thread, waiting on a job the engine does not schedule in Job::wait (state 1-3: waiting for its
//    dependencies, queued, running), wakes the extra worker as Job::wait does before it blocks, naps on the job's
//    state up to mainSpinUs, then calls the engine's Job::wait (a job done meanwhile: its own fast path, as for a
//    job finished before the call). Not for the callers whose waits are long (the average of their last ones).
// Which thread runs which job never changes the results (verified for physics: the jobs and the items in each are
// decided by the code that makes them); the jobs, their number and their contents are untouched.
using PFN_SemWait = bool(__fastcall*)(void* sem, DWORD ms);
PFN_SemWait Real_SemWait = nullptr;
using PFN_SetEventThunk = void(__fastcall*)(void* handle);
PFN_SetEventThunk g_setEventThunk = nullptr;   // the engine's SetEvent(*(HANDLE*)p)
BYTE* g_semWaitRet = nullptr;     // in the worker loop, right after its call of the semaphore wait
BYTE** g_jobMgr = nullptr;        // the engine's JobManager pointer
DWORD* g_engineTls = nullptr;     // the engine's TLS slot: its block +8 = job thread index (-1: not a job thread)
constexpr uint32_t kJmSem = 0x100;          // JobManager: the workers' semaphore (its HANDLE first); +0: the token count
constexpr uint32_t kJmRunning = 4;          // ... running (0: shutting down)
constexpr uint32_t kJmExtraEvent = 0x150;   // ... the extra worker's event
constexpr uint32_t kJobState = 8;           // Job: 0 new, 1 waiting for dependencies, 2 queued, 3 running, 4 done
constexpr unsigned kMwaitxHint = 0xF0;      // MWAITX EAX hint: no C-state (the shortest wake, as Linux's delay_mwaitx)

bool g_spinWorkers = false;       // the semaphore wait hooked
bool g_spinMain = false;          // Job::wait hooked, its wake of the extra worker found
double g_tscPerUs = 0;            // RDTSC ticks per microsecond (invariant TSC)
bool g_mwaitx = false;            // MONITORX / MWAITX usable (AMD: CPUID, then a test under SEH)
uint64_t g_napTicks = 0;          // longest nap: the loops look at the clock, the window and the main thread again
uint64_t g_pollTicks = 0;         // a spinning worker tries the semaphore that often anyway (a raise not seen)...
uint64_t g_fastPollTicks = 0;     // ... and this soon after a raise of the count (ReleaseSemaphore follows the CAS),
uint64_t g_graceTicks = 0;        //     doubling, for that long
BYTE g_coreOf[64];                // logical processor (group 0) -> its physical core (0xFF: unknown)
bool g_coreMap = false;

// Settings: the ini, the control file over it (main thread); what the threads read, set at each frame A.
int g_spinIni = 0;
int g_spinFile = -1, g_spinFileUs = -1, g_spinFileMainUs = -1;   // the control file's (-1: none)
std::atomic<uint64_t> g_spinUntil{ 0 };       // RDTSC: frame A of the headset session runs (0: its Present came)
std::atomic<uint64_t> g_spinWorkerMax{ 0 };   // longest spin of an idle worker, ticks (0: they sleep at once)
std::atomic<uint64_t> g_spinMainMax{ 0 };     // ... of the main thread in Job::wait
std::atomic<DWORD> g_spinMainTid{ 0 };
std::atomic<int> g_spinMainCpu{ -1 };         // logical processor the main thread last ran on
std::atomic<int> g_spinMainWaits{ 0 };       // the main thread is in Job::wait (its core is free): depth
int g_spinFramesA = 0;                        // frames A with spinning since the last stats line
int g_spinPresents = 0;

inline uint64_t SpinTicks(double us) { return (uint64_t)(us * g_tscPerUs); }

// Per job thread, on its own cache line (summed for the stats line: a little stale is fine).
struct alignas(64) WorkerSpin
{
    uint64_t idle = 0;         // waits on the semaphore in frame A
    uint64_t woken = 0;        // ... that took a unit while spinning: no sleep
    uint64_t slept = 0;        // ... that spun, then slept (the engine's INFINITE wait)
    uint64_t skipped = 0;      // ... not spun: beside the running main thread, or frame A over
    uint64_t ticks = 0;        // time spent spinning
    uint64_t wokenTicks = 0;   // ... in the spins that took a unit
};
constexpr int kSpinThreads = 32;
WorkerSpin g_workerSpin[kSpinThreads];
WorkerSpin g_workerSpinLast;                  // their sums at the last stats line
std::atomic<int> g_workerSpinCount{ 0 };
thread_local int t_spinSlot = -1;
thread_local uint64_t t_spinBudget = 0;       // this worker's spin length now

// The main thread's Job::wait in frame A (main thread only).
struct MainSpin { uint64_t waits = 0, caught = 0, blocked = 0, longWaits = 0, other = 0, ticks = 0, caughtTicks = 0; };
MainSpin g_mainSpin;
// Its callers (return address): the average length of their last waits, ticks.
struct SpinSite { void* ret = nullptr; uint64_t avg = 0; uint32_t n = 0; };
constexpr int kSpinSites = 32;
SpinSite g_spinSites[kSpinSites];
int g_spinSiteCount = 0;
SpinSite g_spinSiteOther;   // (the callers past the table, together)

SpinSite& SpinSiteOf(void* ret)
{
    for (int i = 0; i < g_spinSiteCount; ++i)
        if (g_spinSites[i].ret == ret) return g_spinSites[i];
    if (g_spinSiteCount < kSpinSites) {
        g_spinSites[g_spinSiteCount].ret = ret;
        return g_spinSites[g_spinSiteCount++];
    }
    return g_spinSiteOther;
}

// The job thread index the engine keeps in its TLS (-1: not a job thread): what decides Job::wait's wake of the
// extra worker (+5026f8).
inline int EngineThreadIndex()
{
    BYTE* const* tls = (BYTE* const*)__readgsqword(0x58);
    return *(const int*)(tls[*g_engineTls] + 8);
}

// A short wait: MWAITX on *p's cache line (until a write to it, or `ticks`), else a few PAUSEs. None if *p is no
// longer `seen` once the monitor is armed (that write would not wake it).
inline void SpinNap(volatile LONG* p, LONG seen, uint64_t ticks)
{
    if (g_mwaitx) {
        _mm_monitorx((const void*)p, 0, 0);
        if (*p != seen) return;
        _mm_mwaitx(2 /* ECX bit 1: EBX is a timeout */, kMwaitxHint, (unsigned)std::min(ticks, g_napTicks));
    } else {
        for (int i = 0; i < 8; ++i) _mm_pause();
    }
}

// This worker is on the other logical processor of the main thread's core while the main thread runs: a spin there
// would slow the main thread down (one core, shared). Without the core map: whenever the main thread runs.
inline bool BesideMain()
{
    if (g_spinMainWaits.load(std::memory_order_relaxed)) return false;
    if (!g_coreMap) return true;
    const int m = g_spinMainCpu.load(std::memory_order_relaxed);
    const DWORD me = GetCurrentProcessorNumber();
    return m < 0 || m >= 64 || me >= 64 || g_coreOf[me] == g_coreOf[m];
}

// The workers' wait on the job semaphore (+5111d0: WaitForSingleObject(*sem, ms) == WAIT_OBJECT_0), called by the
// worker loop only (+502208), after it took a token there was none of.
bool __fastcall Hook_SemWait(void* sem, DWORD ms)
{
    const uint64_t maxT = g_spinWorkerMax.load(std::memory_order_relaxed);
    const uint64_t until = g_spinUntil.load(std::memory_order_relaxed);
    if (!maxT || !until || ms != INFINITE || (BYTE*)_ReturnAddress() != g_semWaitRet) return Real_SemWait(sem, ms);
    BYTE* jm = *g_jobMgr;
    if (!jm || (BYTE*)sem != jm + kJmSem) return Real_SemWait(sem, ms);
    if (t_spinSlot < 0) t_spinSlot = g_workerSpinCount.fetch_add(1, std::memory_order_relaxed);
    if (t_spinSlot >= kSpinThreads) return Real_SemWait(sem, ms);
    WorkerSpin& st = g_workerSpin[t_spinSlot];
    ++st.idle;
    const uint64_t t0 = __rdtsc();
    if (t0 >= until || BesideMain()) {
        ++st.skipped;
        return Real_SemWait(sem, ms);
    }
    if (!t_spinBudget || t_spinBudget > maxT) t_spinBudget = maxT;
    const uint64_t end = t0 + t_spinBudget;
    volatile LONG* count = (volatile LONG*)jm;
    const volatile BYTE* running = jm + kJmRunning;
    LONG last = *count;
    uint64_t now = t0, poll = t0 + g_pollTicks, grace = 0, gap = g_fastPollTicks;
    bool got = false;
    for (;;) {
        const LONG c = *count;
        if (c > last) {
            // A signal raised the count (its ReleaseSemaphore follows): try at once, then a few times, sooner first.
            grace = now + g_graceTicks;
            gap = g_fastPollTicks;
            poll = now;
        }
        last = c;
        if (now >= poll) {
            // The engine's own wait with a timeout of 0: a unit is taken as its INFINITE wait would take it; none,
            // nothing is done.
            if (Real_SemWait(sem, 0)) {
                got = true;
                break;
            }
            if (now < grace) {
                poll = now + gap;
                gap *= 2;
            } else {
                poll = now + g_pollTicks;
            }
        }
        if (now >= end || !*running || now >= g_spinUntil.load(std::memory_order_relaxed) || BesideMain()) break;
        SpinNap(count, c, std::min(end, poll) - now);
        now = __rdtsc();
    }
    const uint64_t spun = __rdtsc() - t0;
    st.ticks += spun;
    if (got) {
        ++st.woken;
        st.wokenTicks += spun;
        t_spinBudget = maxT;
        return true;
    }
    ++st.slept;
    t_spinBudget = std::max(maxT / 8, t_spinBudget / 2);
    return Real_SemWait(sem, ms);
}

// One Job::wait of the main thread in frame A: what JobSpinBegin saw and did, for JobSpinEnd. (Two halves around
// the engine's call, so Hook_JobWait's timed call stays as it is.)
struct MainSpinCall
{
    SpinSite* site = nullptr;   // its caller; nullptr: another thread, or frame A over - nothing to do afterwards
    uint64_t t0 = 0, t1 = 0;    // RDTSC: the call, the end of the spin
    LONG s = 4;                 // the job's state when the spin ended
    bool spun = false;
};

// Before the engine's Job::wait (any thread; only the main thread in frame A spins).
MainSpinCall JobSpinBegin(void* job, void* ret)
{
    MainSpinCall c;
    if (!job || GetCurrentThreadId() != g_spinMainTid.load(std::memory_order_relaxed)) return c;
    c.t0 = __rdtsc();
    if (c.t0 >= g_spinUntil.load(std::memory_order_relaxed)) return c;
    g_spinMainWaits.fetch_add(1, std::memory_order_relaxed);
    c.site = &SpinSiteOf(ret);
    const uint64_t maxT = g_spinMainMax.load(std::memory_order_relaxed);
    if (maxT) {
        volatile LONG* state = (volatile LONG*)((BYTE*)job + kJobState);
        c.s = *state;
        ++g_mainSpin.waits;
        if (c.s < 1 || c.s > 3 || EngineThreadIndex() != -1) {
            ++g_mainSpin.other;       // 0: Job::wait schedules it itself; 4: done (its fast path)
        } else if (c.site->n >= 4 && c.site->avg > 4 * maxT) {
            ++g_mainSpin.longWaits;   // this caller's waits are long: it blocks at once, as before
        } else {
            // What Job::wait does before it blocks (+5026fe): the extra worker takes the main thread's place.
            g_setEventThunk(*g_jobMgr + kJmExtraEvent);
            c.spun = true;
            const uint64_t end = c.t0 + maxT;
            for (uint64_t now = c.t0; c.s != 4 && now < end; now = __rdtsc()) {
                SpinNap(state, c.s, end - now);
                c.s = *state;
            }
        }
    }
    c.t1 = __rdtsc();
    return c;
}

// Its spin, ms (0: none) - Hook_JobWait adds it to the wait it times.
double SpinMs(const MainSpinCall& c) { return c.site ? (double)(c.t1 - c.t0) / g_tscPerUs / 1000.0 : 0.0; }

// Right after the engine's Job::wait: the counters, this caller's average.
void JobSpinEnd(const MainSpinCall& c)
{
    if (!c.site) return;
    const uint64_t t2 = __rdtsc();
    g_spinMainWaits.fetch_sub(1, std::memory_order_relaxed);
    g_spinMainCpu.store((int)GetCurrentProcessorNumber(), std::memory_order_relaxed);
    if (c.spun) {
        g_mainSpin.ticks += c.t1 - c.t0;
        if (c.s == 4) {
            ++g_mainSpin.caught;
            g_mainSpin.caughtTicks += c.t1 - c.t0;
        } else {
            ++g_mainSpin.blocked;
        }
    }
    // (the whole wait, spin included: what the next decision for this caller looks at)
    SpinSite& site = *c.site;
    const int64_t d = (int64_t)(t2 - c.t0) - (int64_t)site.avg;
    site.avg = site.n ? (uint64_t)((int64_t)site.avg + d / 8) : t2 - c.t0;
    ++site.n;
}

// Job::wait for the calls Hook_JobWait does not time (no perfTrace, other threads): the spin, the engine's function.
__int64 JobWaitSpun(PFN_Any4 real, void* job, void* a2, void* a3, void* a4, void* ret)
{
    const MainSpinCall c = JobSpinBegin(job, ret);
    const __int64 r = real(job, a2, a3, a4);
    JobSpinEnd(c);
    return r;
}

// "key=N" in the control file (digits; spaces allowed), -1 if absent.
int SpinKey(const char* s, const char* key)
{
    const size_t len = strlen(key);
    for (const char* p = s; (p = strstr(p, key)) != nullptr; p += len) {
        if (p > s && isalnum((unsigned char)p[-1])) continue;
        const char* q = p + len;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q < '0' || *q > '9') continue;
        int v = 0;
        while (*q >= '0' && *q <= '9' && v < 100000) v = v * 10 + (*q++ - '0');
        return v;
    }
    return -1;
}

// MONITORX / MWAITX run at any privilege level on AMD, but a hypervisor may refuse them: tried here once under SEH
// (main thread), never first in a worker. The median of 8 naps of `ticks` (0: a fault).
uint64_t MwaitxProbe(uint64_t ticks)
{
    alignas(64) static volatile LONG line = 0;
    uint64_t t[8] = {};
    __try {
        for (int i = 0; i < 8; ++i) {
            const uint64_t a = __rdtsc();
            _mm_monitorx((const void*)&line, 0, 0);
            _mm_mwaitx(2, kMwaitxHint, (unsigned)ticks);
            t[i] = __rdtsc() - a;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    for (int i = 1; i < 8; ++i)
        for (int j = i; j > 0 && t[j - 1] > t[j]; --j) std::swap(t[j - 1], t[j]);
    return t[4] ? t[4] : 1;
}

// Which logical processors share a physical core (group 0), for BesideMain.
void SpinCoreMap(int& cores, int& logical)
{
    cores = logical = 0;
    memset(g_coreOf, 0xFF, sizeof(g_coreOf));
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return;
    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
        return;
    for (DWORD off = 0; off + sizeof(DWORD) * 2 <= len;) {
        auto e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (!e->Size) break;
        if (e->Relationship == RelationProcessorCore) {
            for (WORD g = 0; g < e->Processor.GroupCount; ++g) {
                const GROUP_AFFINITY& a = e->Processor.GroupMask[g];
                if (a.Group != 0) continue;
                for (int b = 0; b < 64; ++b)
                    if ((a.Mask >> b) & 1) {
                        g_coreOf[b] = (BYTE)std::min(cores, 254);
                        ++logical;
                    }
            }
            ++cores;
        }
        off += e->Size;
    }
    g_coreMap = logical > 0 && GetActiveProcessorGroupCount() == 1;
}

// InstallHooks: the clock, the naps, the core map, the hooks (jobHooked: Job::wait goes through Hook_JobWait).
void SpinInstall(const Targets& t, bool jobHooked)
{
    auto rva = [](const void* p) { return p ? (unsigned long long)((const BYTE*)p - g_base) : 0ull; };
    g_spinIni = std::clamp(Cfg().jobSpin, 0, 3);
    // The spins are timed with RDTSC: an invariant TSC (CPUID 80000007h EDX bit 8), its rate against QPC (2 ms, once).
    int r[4];
    __cpuid(r, (int)0x80000000);
    const unsigned ext = (unsigned)r[0];
    bool invariant = false, monitorx = false;
    if (ext >= 0x80000007u) {
        __cpuid(r, (int)0x80000007);
        invariant = (r[3] >> 8) & 1;
    }
    if (ext >= 0x80000001u) {
        __cpuid(r, (int)0x80000001);
        monitorx = (r[2] >> 29) & 1;   // MwaitExtended: MONITORX / MWAITX
    }
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    const uint64_t c0 = __rdtsc();
    do QueryPerformanceCounter(&q1);
    while (q1.QuadPart - q0.QuadPart < f.QuadPart / 500);
    const uint64_t c1 = __rdtsc();
    g_tscPerUs = (double)(c1 - c0) * (double)f.QuadPart / ((double)(q1.QuadPart - q0.QuadPart) * 1e6);
    if (!invariant || g_tscPerUs < 500.0 || g_tscPerUs > 20000.0) {
        Log("Perf: job spin NOT available: the TSC is %s (%.0f MHz)", invariant ? "out of range" : "not invariant", g_tscPerUs);
        return;
    }
    g_napTicks = SpinTicks(2.0);
    g_pollTicks = SpinTicks(16.0);
    g_fastPollTicks = SpinTicks(0.5);
    g_graceTicks = SpinTicks(4.0);
    double napUs = 0;
    if (monitorx) {
        napUs = (double)MwaitxProbe(g_napTicks) / g_tscPerUs;
        g_mwaitx = napUs >= 1.0 && napUs <= 20.0;   // (0: a fault; shorter: no wait at all; longer: its timer not kept)
    }
    int cores = 0, logical = 0;
    SpinCoreMap(cores, logical);
    // (set before the hook goes in: the workers call it at once - it only acts once SpinFrameA allows it)
    g_jobMgr = t.jobManager;
    g_semWaitRet = t.workerWaitRet;
    g_spinWorkers = t.workerWait && t.workerWaitRet && t.jobManager &&
                    HookFunction(t.workerWait, (void*)Hook_SemWait, (void**)&Real_SemWait);
    if (jobHooked && t.jobManager && t.tlsIndex && t.setEventThunk) {
        g_engineTls = t.tlsIndex;
        g_setEventThunk = (PFN_SetEventThunk)t.setEventThunk;
        g_spinMain = true;
    }
    char nap[64];
    if (g_mwaitx) snprintf(nap, sizeof(nap), "MWAITX (2 us asked: %.1f us)", napUs);
    else snprintf(nap, sizeof(nap), "PAUSE (MWAITX %s)", !monitorx ? "not offered" : napUs <= 0 ? "faulted"
                                                          : napUs < 1.0 ? "did not wait" : "kept no timer");
    Log("Perf: job spin (ini jobSpin=%d, jobSpinUs=%d, mainSpinUs=%d): workers' semaphore wait +%llx %s (its call +%llx), "
        "Job::wait +%llx %s; TSC %.0f MHz; naps %s; %d cores, %d logical processors%s", g_spinIni, Cfg().jobSpinUs,
        Cfg().mainSpinUs, rva(t.workerWait), g_spinWorkers ? "hooked" : "NOT hooked",
        rva(t.workerWaitRet ? t.workerWaitRet - 5 : nullptr), rva(t.jobWait), g_spinMain ? "hooked" : "NOT available",
        g_tscPerUs, nap, cores, logical, g_coreMap ? "" : " (core map unknown: workers spin only while the main thread waits)");
}

// ---------------------------------------------------------------------------------------------------
// Hooks (every one calls the engine's function through, unchanged)
// ---------------------------------------------------------------------------------------------------
PFN_Any4 Real_JobWait = nullptr;
PFN_Any4 Real_FenceWait = nullptr;
using PFN_DisplayWait = void(__fastcall*)(void* swapChain);
PFN_DisplayWait Real_DisplayWait = nullptr;
using PFN_ProfAdd = void(__fastcall*)(void* prof, int cat, float seconds);
PFN_ProfAdd Real_ProfAdd = nullptr;
using PFN_ModTick = __int64(__fastcall*)(BYTE* ent, float dt, void* input, void* a4);
PFN_ModTick Real_ModTick = nullptr;
using PFN_Dispatch = __int64(__fastcall*)(unsigned __int64 realm, const char* name, void* impl, void* script, void* args,
                                          void* ret);
PFN_Dispatch Real_Dispatch = nullptr;

// Function of teardown.exe a return address belongs to (its RVA; primary entry of chained
// unwind info), cached per return address.
struct RetRva { void* ret; DWORD rva; };
RetRva g_retCache[32];
int g_retCacheN = 0;

DWORD CallerRva(void* ret)
{
    for (int i = 0; i < g_retCacheN; ++i)
        if (g_retCache[i].ret == ret) return g_retCache[i].rva;
    DWORD rva = (DWORD)((BYTE*)ret - g_base);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)ret, &imageBase, nullptr);
    if (f && (BYTE*)imageBase == g_base) {
        for (int k = 0; k < 8; ++k) {
            const BYTE* u = g_base + f->UnwindData;
            if (!((u[0] >> 3) & UNW_FLAG_CHAININFO)) break;
            const int codes = u[2];
            f = (PRUNTIME_FUNCTION)(u + 4 + ((codes + 1) & ~1) * 2);
        }
        rva = f->BeginAddress;
    }
    if (g_retCacheN < 32) g_retCache[g_retCacheN++] = { ret, rva };
    return rva;
}

__int64 __fastcall Hook_JobWait(void* job, void* a2, void* a3, void* a4)
{
    void* ret = _ReturnAddress();
    // ([Performance] jobSpin: in frame A of the headset session the main thread may first spin a few microseconds
    // before the engine's Job::wait - JobSpinBegin / JobSpinEnd. Hooked without perfTrace too: then only that.)
    if (g_perfTrace < 1 || !OnMain()) return JobWaitSpun(Real_JobWait, job, a2, a3, a4, ret);
    const MainSpinCall spin = JobSpinBegin(job, ret);
    // (perfDetail 1: the job's state before the call - 4 = done, nothing to wait for)
    const int state = (g_detail.load(std::memory_order_relaxed) & kDetailWaits) ? *(volatile const int*)((BYTE*)job + 8) : -1;
    const double t0 = ProfNow();
    __int64 r = Real_JobWait(job, a2, a3, a4);
    const double ms = SpinMs(spin) + (ProfNow() - t0);   // (jobSpin: the spin before the call is part of this wait)
    JobSpinEnd(spin);
    g_prof[P_JobWait] += ms;
    NoteJobWait(state, ms);
    if (ms > 0.002) {
        const DWORD rva = CallerRva(ret);
        int i = 0;
        while (i < g_waitFrameN && g_waitFrame[i].rva != rva) ++i;
        if (i == g_waitFrameN) {
            if (g_waitFrameN < kWaitFrame - 1) {
                g_waitFrame[g_waitFrameN++] = { rva, 0, 0 };
            } else {
                // (table full: this caller goes with the others in the last entry, "+other")
                i = kWaitFrame - 1;
                if (g_waitFrameN < kWaitFrame) g_waitFrame[g_waitFrameN++] = { 0, 0, 0 };
            }
        }
        g_waitFrame[i].ms += ms;
        ++g_waitFrame[i].n;
    }
    return r;
}

// perfDetail 1: a GPU fence wait of the main thread, by calling function. The engine's callers of
// TFenceD3D12::waitValueOnCpu (Teardown 2.0.1): TRendererD3D12::beginFrame +5b35a0 (the frame context of two
// frames back), the async uploader's waitForCurrentBuffer +5ae070 (its 32 MB staging buffer still in
// flight), waitIdle +5ba2b0 (level change, rebuilds), the swap chain resize +5bbcd0.
void NoteFenceWait(void* ret, double ms)
{
    if (!(g_detail.load(std::memory_order_relaxed) & kDetailWaits) || ms < 0.002) return;
    AddBy(g_detFrame.fence, g_detFrame.fenceN, kFenceBy, CallerRva(ret), ms, 1);
}

__int64 __fastcall Hook_FenceWait(void* fence, void* value, void* a3, void* a4)
{
    if (!OnMain()) {
        // (perfDetail 1: the other threads' waits too - the uploader may stage from a job thread)
        if (!(g_detail.load(std::memory_order_relaxed) & kDetailWaits)) return Real_FenceWait(fence, value, a3, a4);
        const double t0 = ProfNow();
        __int64 r = Real_FenceWait(fence, value, a3, a4);
        const double ms = ProfNow() - t0;
        if (ms >= 0.002) {
            g_offFenceUs.fetch_add((uint64_t)(ms * 1000.0), std::memory_order_relaxed);
            g_offFenceN.fetch_add(1, std::memory_order_relaxed);
        }
        return r;
    }
    void* ret = _ReturnAddress();
    const double t0 = ProfNow();
    __int64 r = Real_FenceWait(fence, value, a3, a4);
    const double ms = ProfNow() - t0;
    g_prof[P_GpuWait] += ms;
    NoteFenceWait(ret, ms);
    return r;
}

// The engine's wait on the desktop swap chain's frame-latency object, right after its Present
// (TRendererD3D12::endRender): with a frame latency of 1, until the GPU has finished the frame just
// presented. It falls in the next frame's "rest" (frames are counted from Present to Present).
void __fastcall Hook_DisplayWait(void* swapChain)
{
    if (!OnMain()) return Real_DisplayWait(swapChain);
    const double t0 = ProfNow();
    Real_DisplayWait(swapChain);
    g_prof[P_DisplayWait] += ProfNow() - t0;
}

void __fastcall Hook_ProfAdd(void* prof, int cat, float seconds)
{
    Real_ProfAdd(prof, cat, seconds);
    if ((unsigned)cat < (unsigned)kCats && OnMain()) g_catFrame[cat] += (double)seconds * 1000.0;
}

// The two costliest mod ticks of the current frame A, named on its "Slow frame" line (real headset sessions
// showed most slow frames dominated by "update+scripts": which mod it was). Names are copied: a ModRec can be
// erased later.
struct TopMod { char name[64]; double ms; };
TopMod g_topMod[2] = {};

void NoteTopMod(const ModRec* m, double ms)
{
    for (TopMod& t : g_topMod)   // (the same mod ticked twice in one frame: its total)
        if (t.ms > 0 && strcmp(t.name, m->name) == 0) { ms += t.ms; t.ms = 0; t.name[0] = 0; break; }
    if (g_topMod[1].ms > g_topMod[0].ms) std::swap(g_topMod[0], g_topMod[1]);
    if (ms <= g_topMod[1].ms) return;
    TopMod n{};
    strncpy_s(n.name, m->name, _TRUNCATE);
    n.ms = ms;
    if (ms > g_topMod[0].ms) { g_topMod[1] = g_topMod[0]; g_topMod[0] = n; }
    else g_topMod[1] = n;
}

__int64 __fastcall Hook_ModTick(BYTE* ent, float dt, void* input, void* a4)
{
    if (!ent || !OnMain()) return Real_ModTick(ent, dt, input, a4);
    ModRec* m = ModOf(ent);
    const int eye = g_updEye;
    if (eye == 1 && FrameB::PairMode() == 1) {
        // Frame B mode 1: a mod whose tick in this pair's frame A (the frame just before) called only
        // reading bindings would compute the same instant again - its frame-B tick is skipped (the
        // whole 0x46b060: server and client scripts, the "shared" table copy and the message queue,
        // which only act after a tick). Never ours (its right-eye duties) nor a frameBKeepMods one.
        ++g_skip.ticks;
        if (m->aFrame + 1 == g_frameNo && m->aClean && !m->ours && !m->keep) {
            ++g_skip.skipped;
            g_skip.ms += m->aMs;
            m->seen = g_frameNo;
            return 0;
        }
    }
    ModRec* outer = g_tickMod;
    g_tickMod = m;
    if (eye == 0) {
        m->aFrame = g_frameNo;
        m->aClean = true;   // (until a binding that is not a read)
    }
    const double t0 = ProfNow();
    __int64 r = Real_ModTick(ent, dt, input, a4);
    const double ms = ProfNow() - t0;
    if (eye == 0) {
        m->aMs = ms;
        NoteTopMod(m, ms);
    }
    m->ms[eye] += ms;
    ++m->calls[eye];
    m->seen = g_frameNo;
    g_tickMod = outer;
    return r;
}

// A script is calling a binding: a registry write in it is the script's (frame B mode 2 records the
// keys scripts write - DisableMotionBlur, SetBool... - not the engine's own; frameb.h).
struct ScriptWrite
{
    ScriptWrite() { FrameB::ScriptWriteBegin(); }
    ~ScriptWrite() { FrameB::ScriptWriteEnd(); }   // (also when a Lua error unwinds through here)
};

__int64 __fastcall Hook_Dispatch(unsigned __int64 realm, const char* name, void* impl, void* script, void* args, void* ret)
{
    if (!OnMain()) return Real_Dispatch(realm, name, impl, script, args, ret);
    const int slot = SlotOf(name);
    const int eye = g_updEye;
    // (recorded before the call: a binding that raises a Lua error does not come back here)
    if (g_tickMod) {
        if (slot >= 0) g_tickMod->used[eye].set(slot);
        if (eye == 0 && (slot < 0 || !g_slotRead[slot])) g_tickMod->aClean = false;
    }
    ScriptWrite w;
    if (g_perfTrace < 2) return Real_Dispatch(realm, name, impl, script, args, ret);   // (frame B work only)
    const double t0 = ProfNow();
    __int64 r = Real_Dispatch(realm, name, impl, script, args, ret);
    if (slot >= 0) {
        g_bind[eye][slot].ms += ProfNow() - t0;
        ++g_bind[eye][slot].calls;
    }
    return r;
}

// Registry setters and the other functions of the core list (not through the dispatcher): one
// detour per function, the census (each of them is a write: a mod calling one keeps its frame-B
// tick in mode 1), and for the registry setters the frame-B mode 2 record (frameb.h: a script's
// registry write, told from the engine's own).
constexpr int kMaxCore = 32;
PFN_Any4 g_coreReal[kMaxCore] = {};
int g_coreSlot[kMaxCore] = {};
bool g_coreRegWrite[kMaxCore] = {};   // SetBool / SetInt / SetFloat / SetString / ClearKey
const char* g_coreName[kMaxCore] = {};

template<int N> __int64 __fastcall Hook_Core(void* script, void* args, void* ret, void* a4)
{
    if (!OnMain()) return g_coreReal[N](script, args, ret, a4);
    if (g_tickMod) {
        if (g_coreSlot[N] >= 0) g_tickMod->used[g_updEye].set(g_coreSlot[N]);
        if (g_updEye == 0) g_tickMod->aClean = false;
    }
    if (!g_coreRegWrite[N]) return g_coreReal[N](script, args, ret, a4);
    ScriptWrite w;
    return g_coreReal[N](script, args, ret, a4);
}
const PFN_Any4 kCoreHooks[kMaxCore] = {
    Hook_Core<0>, Hook_Core<1>, Hook_Core<2>, Hook_Core<3>, Hook_Core<4>, Hook_Core<5>, Hook_Core<6>, Hook_Core<7>,
    Hook_Core<8>, Hook_Core<9>, Hook_Core<10>, Hook_Core<11>, Hook_Core<12>, Hook_Core<13>, Hook_Core<14>, Hook_Core<15>,
    Hook_Core<16>, Hook_Core<17>, Hook_Core<18>, Hook_Core<19>, Hook_Core<20>, Hook_Core<21>, Hook_Core<22>, Hook_Core<23>,
    Hook_Core<24>, Hook_Core<25>, Hook_Core<26>, Hook_Core<27>, Hook_Core<28>, Hook_Core<29>, Hook_Core<30>, Hook_Core<31>,
};
static_assert(sizeof(kCoreHooks) / sizeof(kCoreHooks[0]) == sizeof(Targets::core) / sizeof(Targets::core[0]),
              "one detour per entry of Perf::Targets::core");

// ---------------------------------------------------------------------------------------------------
// CPU per thread group (background thread, every second)
// ---------------------------------------------------------------------------------------------------
enum CpuGroup { G_Main, G_Jobs, G_Engine, G_Ours, G_Other, G_Count };
struct CpuAcc
{
    double cpu[G_Count] = {};   // seconds of CPU
    double process = 0;
    double busy = 0, total = 0; // system: busy and total CPU time (all CPUs)
    double wall = 0;            // seconds
    int jobThreads = 0;
};
std::mutex g_cpuMutex;
CpuAcc g_cpu;

double FileTimeSec(const FILETIME& f) { return (double)(((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) * 1e-7; }

DWORD WINAPI CpuThread(LPVOID)
{
    using PFN_NtQIT = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    auto ntQuery = (PFN_NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    HMODULE ours = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&CpuThread, &ours);
    auto range = [](HMODULE m, BYTE*& lo, BYTE*& hi) {
        lo = (BYTE*)m;
        hi = lo;
        if (!m) return;
        auto nt = (IMAGE_NT_HEADERS*)(lo + ((IMAGE_DOS_HEADER*)lo)->e_lfanew);
        hi = lo + nt->OptionalHeader.SizeOfImage;
    };
    BYTE *gameLo, *gameHi, *oursLo, *oursHi;
    range(GetModuleHandleW(nullptr), gameLo, gameHi);
    range(ours, oursLo, oursHi);

    struct Th { DWORD tid; HANDLE h; BYTE* start; int prio; int group; double last; bool seen; };
    std::vector<Th> ths;
    ULONGLONG listAt = 0;
    const DWORD pid = GetCurrentProcessId();
    auto threadSec = [](HANDLE h) {
        FILETIME c, e, k, u;
        return GetThreadTimes(h, &c, &e, &k, &u) ? FileTimeSec(k) + FileTimeSec(u) : -1.0;
    };
    FILETIME c, e, k, u, si, sk, su;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    double procLast = FileTimeSec(k) + FileTimeSec(u);
    GetSystemTimes(&si, &sk, &su);
    double idleLast = FileTimeSec(si), sysLast = FileTimeSec(sk) + FileTimeSec(su);
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    double wallLast = (double)t.QuadPart / f.QuadPart;
    for (;;) {
        Sleep(1000);
        const ULONGLONG now = GetTickCount64();
        if (ths.empty() || now - listAt >= 10000) {
            // The threads of the game (again every 10 s: threads come and go).
            listAt = now;
            for (Th& th : ths) th.seen = false;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te = { sizeof(te) };
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != pid) continue;
                    auto it = std::find_if(ths.begin(), ths.end(), [&](const Th& x) { return x.tid == te.th32ThreadID; });
                    if (it != ths.end()) {
                        it->seen = true;
                        continue;
                    }
                    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (!h) continue;
                    void* start = nullptr;
                    if (ntQuery) ntQuery(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
                    const double s = threadSec(h);
                    ths.push_back({ te.th32ThreadID, h, (BYTE*)start, GetThreadPriority(h), G_Other, s < 0 ? 0 : s, true });
                }
                CloseHandle(snap);
            }
            for (size_t i = 0; i < ths.size();) {
                if (!ths[i].seen) {
                    CloseHandle(ths[i].h);
                    ths[i] = ths.back();
                    ths.pop_back();
                } else {
                    ++i;
                }
            }
        }
        // Groups (again each time: the main thread is known after the first game update). Job
        // workers: started in teardown.exe at THREAD_PRIORITY_BELOW_NORMAL (they set it themselves).
        const DWORD mainTid = g_mainTid;
        double sum[G_Count] = {};
        int jobs = 0;
        for (Th& th : ths) {
            int g = G_Other;
            if (th.tid == mainTid) g = G_Main;
            else if (th.start >= gameLo && th.start < gameHi) g = th.prio == THREAD_PRIORITY_BELOW_NORMAL ? G_Jobs : G_Engine;
            else if (th.start >= oursLo && th.start < oursHi) g = G_Ours;
            th.group = g;
            if (g == G_Jobs) ++jobs;
            const double s = threadSec(th.h);
            if (s < 0) continue;
            sum[g] += std::max(0.0, s - th.last);
            th.last = s;
        }
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        const double proc = FileTimeSec(k) + FileTimeSec(u);
        GetSystemTimes(&si, &sk, &su);
        const double idle = FileTimeSec(si), sys = FileTimeSec(sk) + FileTimeSec(su);   // (kernel time includes idle)
        QueryPerformanceCounter(&t);
        const double wall = (double)t.QuadPart / f.QuadPart;
        {
            std::lock_guard<std::mutex> lock(g_cpuMutex);
            for (int i = 0; i < G_Count; ++i) g_cpu.cpu[i] += sum[i];
            g_cpu.process += std::max(0.0, proc - procLast);
            g_cpu.total += std::max(0.0, sys - sysLast);
            g_cpu.busy += std::max(0.0, (sys - sysLast) - (idle - idleLast));
            g_cpu.wall += wall - wallLast;
            g_cpu.jobThreads = jobs;
        }
        procLast = proc;
        idleLast = idle;
        sysLast = sys;
        wallLast = wall;
    }
}

// ---------------------------------------------------------------------------------------------------
// Priority, overlays
// ---------------------------------------------------------------------------------------------------
bool g_raised = false;           // we set ABOVE_NORMAL (and restore NORMAL)
bool g_sessionLast = false;
std::atomic<const char*> g_overlay{ nullptr };

void AppendF(std::string& s, const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(b, (size_t)std::min(n, (int)sizeof(b) - 1));
}

// " +4304d0 1.23" (fmt: the format of ms); the callers together past the table: " +other 1.23".
void AppendWait(std::string& s, const char* sep, const WaitBy& w, double ms, const char* fmt)
{
    char num[32];
    snprintf(num, sizeof(num), fmt, ms);
    if (w.rva) AppendF(s, "%s+%x %s", sep, w.rva, num);
    else AppendF(s, "%s+other %s", sep, num);
}

// Logs text in lines Log can hold (its text: 2047 characters), cut at spaces; the lines after
// the first start with cont.
void LogSplit(const std::string& text, const std::string& cont)
{
    constexpr size_t kMax = 1900;
    size_t pos = 0;
    for (bool first = true; pos < text.size(); first = false) {
        const size_t room = kMax - (first ? 0 : cont.size());
        size_t end = text.size();
        if (end - pos > room) {
            end = text.rfind(' ', pos + room);
            if (end == std::string::npos || end <= pos) end = pos + room;
        }
        Log("%s%s", first ? "" : cont.c_str(), text.substr(pos, end - pos).c_str());
        pos = end;
        while (pos < text.size() && text[pos] == ' ') ++pos;
    }
}

// ---------------------------------------------------------------------------------------------------
// perfDetail 1: the async uploader's bytes (TAsyncDataUploaderD3D12::submit)
// ---------------------------------------------------------------------------------------------------
// submit(uploader, mode, queue) closes the current staging buffer's copy list and runs it on the
// uploader's own COPY queue: mode 2 is endRender's, once per frame (then a GPU-side wait on the game's
// queue); mode 0 comes from an upload that filled the 32 MB buffer mid-frame ("forced": the next upload
// may then wait in waitForCurrentBuffer); mode 1 submits and waits at once (waitIdle at a level change,
// the renderer's and the uploader's shutdown) - counted, not as forced.
// What it sends = the buffer's used bytes, read before the call. The buffer is the 0x38-byte record
// [uploader+48h] + [uploader+0C0h] x 38h: used +10h (capacity [uploader+38h]), state +30h (1 = data staged)
// - Teardown 2.0.1, from 0x5ad5a0 (upload), 0x5ad030 (submit) and 0x5ae070 (reuse).
PFN_Any4 Real_UpSubmit = nullptr;

__int64 __fastcall Hook_UpSubmit(void* up, void* mode, void* a3, void* a4)
{
    if (g_detail.load(std::memory_order_relaxed) & kDetailWaits) {
        const BYTE* u = (const BYTE*)up;
        const BYTE* rec = *(BYTE* const*)(u + 0x48) + (ptrdiff_t)*(const int*)(u + 0xc0) * 0x38;
        const uint32_t used = *(const uint32_t*)(rec + 0x10), cap = *(const uint32_t*)(u + 0x38);
        if (*(const int*)(rec + 0x30) == 1 && used <= cap) {
            g_upBytes.fetch_add(used, std::memory_order_relaxed);
            g_upSubmits.fetch_add(1, std::memory_order_relaxed);
            if ((int)(intptr_t)mode == 0) g_upForced.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return Real_UpSubmit(up, mode, a3, a4);
}

// ---------------------------------------------------------------------------------------------------
// perfDetail 2: every job timed by class (Job::run, all threads)
// ---------------------------------------------------------------------------------------------------
// Job::run (0x501b20) runs the job's body (vtable[1]), marks it done, wakes its waiter, then its
// dependants: those marked "inline" (a group's barrier, TSyncJob) run inside it, recursively; the others
// are queued. Each run is timed with the TSC around the call, minus the runs nested in it (its own time),
// and summed per class (the job's vtable) and per eye of the main thread's frame when it started. Every
// thread sums into its own table; the only shared write is a class's longest run of the frame (a CAS, only
// when longer). The engine's call runs as it was: same job, same thread, same order.
using PFN_JobRun = void(__fastcall*)(BYTE* job);
PFN_JobRun Real_JobRun = nullptr;
constexpr int kJobTypes = 128;          // classes, by vtable (open addressing); index kJobTypes: the others
constexpr int kJobThreads = 48;         // threads with a table (the engine has 16 job threads, + main)
constexpr int kJobDepth = 8;            // nested runs timed (deeper ones count in their parent)
struct JobCell { uint64_t ticks = 0, max = 0; uint32_t n = 0; };
struct JobAcc { JobCell c[2][2][kJobTypes + 1]; };   // [window: g_jobGen][eye][class]
std::atomic<void*> g_jobVt[kJobTypes];
std::atomic<int> g_jobTypeN{ 0 };
std::atomic<uint64_t> g_jobFrameMax[kJobTypes + 1];  // each class's longest run in this engine frame (TSC)
std::atomic<JobAcc*> g_jobAcc[kJobThreads];
std::atomic<int> g_jobAccN{ 0 };
std::atomic<int> g_jobGen{ 0 };         // the window the threads add to (flipped at each statistics line)
std::atomic<int> g_jobEye{ 0 };         // eye of the main thread's frame (UpdateBegin): 0 = A, 1 = B
thread_local JobAcc* t_jobAcc = nullptr;
thread_local bool t_jobNoAcc = false;   // (past kJobThreads threads, or no memory: this thread not counted)
thread_local int t_jobDepth = 0;
thread_local uint64_t t_jobChild[kJobDepth];   // time of the runs nested in the run at each depth
char g_jobName[kJobTypes][64];          // their names, resolved once (main thread, LogInstrument)
double g_tscPerMs = 0;                  // TSC ticks per ms, measured with QPC over each window
uint64_t g_tscLast = 0;
double g_msLast = 0;

int JobClass(void* vt)
{
    size_t h = (size_t)(((uintptr_t)vt >> 3) * 0x9E3779B97F4A7C15ull >> 57);   // 7 bits
    for (int i = 0; i < kJobTypes; ++i, h = (h + 1) & (kJobTypes - 1)) {
        void* cur = g_jobVt[h].load(std::memory_order_acquire);
        if (cur == vt) return (int)h;
        if (cur) continue;
        if (g_jobTypeN.load(std::memory_order_relaxed) >= kJobTypes * 3 / 4) return kJobTypes;
        if (g_jobVt[h].compare_exchange_strong(cur, vt, std::memory_order_acq_rel)) {
            g_jobTypeN.fetch_add(1, std::memory_order_relaxed);
            return (int)h;
        }
        if (cur == vt) return (int)h;   // (another thread put it there meanwhile)
    }
    return kJobTypes;
}

JobAcc* JobAccOfThread()
{
    if (t_jobAcc || t_jobNoAcc) return t_jobAcc;
    const int i = g_jobAccN.fetch_add(1, std::memory_order_relaxed);
    JobAcc* a = i < kJobThreads ? new (std::nothrow) JobAcc() : nullptr;
    if (!a) {
        t_jobNoAcc = true;
        return nullptr;
    }
    g_jobAcc[i].store(a, std::memory_order_release);
    return t_jobAcc = a;
}

void __fastcall Hook_JobRun(BYTE* job)
{
    if (!(g_detail.load(std::memory_order_relaxed) & kDetailJobs) || t_jobDepth >= kJobDepth) return Real_JobRun(job);
    // (read before the run: once it is done its waiter may release the job)
    void* vt = *(void**)job;
    const int eye = g_jobEye.load(std::memory_order_relaxed) & 1;
    const int d = t_jobDepth++;
    t_jobChild[d] = 0;
    const uint64_t t0 = __rdtsc();
    Real_JobRun(job);
    const uint64_t dt = __rdtsc() - t0;
    t_jobDepth = d;
    if (d > 0) t_jobChild[d - 1] += dt;
    JobAcc* a = JobAccOfThread();
    if (!a) return;
    const uint64_t self = dt > t_jobChild[d] ? dt - t_jobChild[d] : 0;
    const int k = JobClass(vt);
    JobCell& c = a->c[g_jobGen.load(std::memory_order_relaxed) & 1][eye][k];
    c.ticks += self;
    ++c.n;
    if (self > c.max) c.max = self;
    uint64_t m = g_jobFrameMax[k].load(std::memory_order_relaxed);
    while (self > m && !g_jobFrameMax[k].compare_exchange_weak(m, self, std::memory_order_relaxed)) {}
}

// The mangled class name of a vtable (MSVC RTTI: vtable[-1] = its complete object locator - x64: signature
// 1, the type descriptor's RVA at +12, the name at +16 in it), copied under SEH.
bool RawClassName(const void* vt, char* out, size_t n)
{
    out[0] = 0;
    const BYTE* end = g_base + g_imageSize;
    if ((const BYTE*)vt < g_base + 8 || (const BYTE*)vt >= end) return false;
    __try {
        const BYTE* col = ((const BYTE* const*)vt)[-1];
        if (col < g_base || col + 24 > end || *(const DWORD*)col != 1) return false;
        const char* s = (const char*)(g_base + *(const DWORD*)(col + 12) + 16);
        if ((const BYTE*)s >= end) return false;
        size_t i = 0;
        for (; i + 1 < n && (const BYTE*)s + i < end && s[i]; ++i) out[i] = s[i];
        out[i] = 0;
        return i > 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

// ".?AVTSolverJob@TSolver@@" -> "TSolver::TSolverJob": the class and the scope it is declared in - a
// class, a template with its first argument (AsyncRange<Physics>), or a function for a local class
// (FireSystem::update()). Enough to tell the engine's job classes apart (a lambda argument: "lambda").
std::string ShortClassName(const char* raw)
{
    const char* p = raw;
    if (strncmp(p, ".?A", 3) == 0 && p[3]) p += 4;   // ".?AV" class, ".?AU" struct
    auto ident = [](const char*& q) {
        std::string r;
        while (*q && *q != '@') r += *q++;
        if (*q == '@') ++q;
        return r;
    };
    auto arg = [&](const char*& q) {   // a template's first argument: "VPhysics@@" -> Physics, a lambda -> lambda
        if (*q != 'V' && *q != 'U') return std::string("?");
        ++q;
        const std::string a = ident(q);
        return a.rfind("<lambda", 0) == 0 ? std::string("lambda") : a;
    };
    if (p[0] == '?' && p[1] == '$') {   // a class template: "?$AsyncJob@V<lambda_..>@@@TJobManager@@"
        p += 2;
        const std::string t = ident(p);
        return t + "<" + arg(p) + ">";
    }
    const std::string name = ident(p);
    std::string scope;
    if (p[0] == '?' && p[1] == '$') {   // in a class template: "RangeTask@?$AsyncRange@VPhysics@@@@"
        p += 2;
        scope = ident(p);
        scope += "<" + arg(p) + ">";
    } else if (p[0] == '?' && p[1] >= '0' && p[1] <= '9') {   // a local class: "@?1??update@FireSystem@@QEAAXM@Z@"
        p += 2;
        while (*p == '?') ++p;
        const bool tmpl = *p == '$';   // (a function template: "?1???$ParallelFor@V<lambda_..>@@@@YAX...")
        if (tmpl) ++p;
        scope = ident(p);
        if (tmpl) scope += "<" + arg(p) + ">";
        else if (*p && *p != '@' && *p != '?') scope = ident(p) + "::" + scope;   // its class
        scope += "()";
    } else if (*p && *p != '@') {   // a class or namespace: "TSolverJob@TSolver@@"
        scope = ident(p);
    }
    return scope.empty() ? name : scope + "::" + name;
}

// A class for the log: "TSolver::TSolverJob" ("+<vtable rva>" without RTTI; the lambdas of templates, which
// share a name, with their vtable's rva).
std::string JobLabel(int k)
{
    if (k >= kJobTypes) return "other classes";
    void* vt = g_jobVt[k].load(std::memory_order_acquire);
    if (!vt) return "?";
    const unsigned long long rva = (unsigned long long)((const BYTE*)vt - g_base);
    char* name = g_jobName[k];
    if (!name[0]) {
        char raw[256];
        if (RawClassName(vt, raw, sizeof(raw))) snprintf(name, sizeof(g_jobName[k]), "%s", ShortClassName(raw).c_str());
        else snprintf(name, sizeof(g_jobName[k]), "+%llx", rva);
    }
    std::string s = name;
    if (s.find("lambda") != std::string::npos) {
        char v[24];
        snprintf(v, sizeof(v), "/+%llx", rva);
        s += v;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------
// perfDetail 4: the GPU's busy and idle time per engine frame (timestamps)
// ---------------------------------------------------------------------------------------------------
// Each ExecuteCommandLists of the main thread on the swap chain's queue - the game's frame (one, at the end
// of TRendererD3D12::endRender) and ours (in the Present: the VR copies, the runtime's) - goes to the GPU
// as [mark, lists..., mark]: two command lists of ours, recorded once at start, each a single timestamp
// (EndQuery). The queue runs them in its order, so a mark's time is when the GPU got there. Per frame:
// busy = the sum of (end - max(start, the end before)), idle = the gaps before the starts (before the
// first: the GPU waiting for the CPU). Nothing drawn changes: the game's lists, their order and their queue
// are the same; ours write only a query heap and a readback buffer of ours. A frame's timestamps are copied
// to the readback buffer by a list of ours put in front of the next submission, and read once our fence,
// signalled right after it, is reached (checked at each Present, never waited on).
constexpr int kGpuRing = 8;             // frames timed and not read yet, at most (the engine has 2-3 in flight)
constexpr int kGpuMarks = 16;           // timestamps per frame: 2 per submission
constexpr int kPassMarks = 128;         // perfDetail 8: timestamps per frame in the game's own command list
struct GpuFrame
{
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* mark[kGpuMarks] = {};         // EndQuery(TIMESTAMP, query i of this frame)
    ID3D12GraphicsCommandList* resolve[kGpuMarks / 2] = {};  // ResolveQueryData of its first 2 (k + 1) queries
    int n = 0;                          // timestamps used
    bool ours[kGpuMarks / 2] = {};      // submission k was made inside the Present
    bool full = false;                  // a submission of the frame went untimed (more than kGpuMarks / 2)
    bool used = false;                  // timed, not read yet
    bool gapBefore = false;             // the frame before it was not timed (the idle gap before it: unknown)
    int eye = 2;
    UINT64 ready = 0;                   // our fence value once its timestamps are in the readback buffer
    // perfDetail 8 (GPU time per pass): timestamps of ours in the game's own command list
    ID3D12GraphicsCommandList* passResolve = nullptr;   // copies this slot's pass timestamps to the readback buffer
    int pn = 0;                         // pass timestamps written
    bool pover = false;                 // more pass changes than kPassMarks: the last pass took the rest
    bool presolved = false;             // their copy rides in the submission our fence (ready) follows
    uint8_t plabel[kPassMarks] = {};    // the pass starting at each one (g_passLabel)
};
struct GpuState
{
    bool tried = false, ok = false;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    double ticksPerMs = 0;
    GpuFrame f[kGpuRing];
    int cur = 0;                        // the frame being timed
    int next = 0;                       // the oldest frame timed and not read yet
    int toResolve = -1;                 // a frame whose timestamps the next submission copies out
    bool gap = true;                    // the last frame was not timed
    UINT64 lastEnd = 0;                 // GPU time of the last frame's last timestamp
    // perfDetail 8: the pass timestamps' heap and readback buffer, and the list submitted once that writes them all
    bool passOk = false, primed = false;
    ID3D12QueryHeap* passHeap = nullptr;
    ID3D12Resource* passReadback = nullptr;
    ID3D12GraphicsCommandList* passPrime = nullptr;
};
GpuState g_gpu;
struct GpuSums
{
    int frames = 0, idleKnown = 0, unmarked = 0;
    double game = 0, ours = 0, idleBefore = 0, idleInside = 0, longest = 0, submissions = 0;
};
GpuSums g_gpuSum[3];
int g_gpuEye = 2;                       // eye of the frame being presented (FrameEnd, earlier in the Present)

// ---------------------------------------------------------------------------------------------------
// perfDetail 8: the GPU time of each pass of the game's frame (timestamps in the game's own command list)
// ---------------------------------------------------------------------------------------------------
// The game records a frame into one command list (TRendererD3D12 +e40, from beginRender to endRender) and
// submits it once (endRender's ExecuteCommandLists, timed whole by 4 above). Each pass starts by setting its
// shader program: TRendererD3D12::setGraphicsProgram / setComputeProgram (vtable +190 / +198; Teardown 2.0.1
// +5b3890 / +5b12e0), hooked here. Where the pass changes (another pass than the previous mark's), one
// EndQuery(TIMESTAMP) of ours is written into that list just before the program is set: the GPU takes it once
// the work recorded before it has gone through. A pass is named by its call site (the return address; Teardown
// 2.0.1's sites below, checked at start), any other site by its calling function ("+rva"). Also marked:
// Renderer::render's start and return (engine.cpp, Hook_Render), its voxel-texture update (+a0500: uploads, then
// the "voxtexturemips" compute) and ShadowVolume's GPU part (+a2870: uploads or the "shadowmips" compute), so that
// the copies they record are not counted in the pass before them. Per frame: the list's start -> the first mark
// ("frame start"), each mark -> the next (that mark's pass), the last mark -> the list's end (4's timestamp after
// the game's submission). The timestamps go to a query heap and a readback buffer of ours, copied out by a list of
// ours in front of the next submission (4's), read once our fence is reached (never waited on). The game's
// commands, their order, list and queue are unchanged: a timestamp changes no state. Cost: one EndQuery per pass
// change (about 30 for the 3D passes, more with the 2D ones; at most kPassMarks per frame), a few microseconds.
constexpr int kPassLabels = 160;        // passes told apart (then "other")
enum { kPassStart = 0, kPassRender, kPassAfter, kPassVoxels, kPassShadow, kPassOther, kPassOutside };
struct PassLabel { char name[40]; };
PassLabel g_passLabel[kPassLabels];
int g_passLabels = 0;
struct PassSite { DWORD ret; const char* name; };
// Teardown 2.0.1: every set-program call site of teardown.exe (its return address) and its pass (design.md of
// gpu_passes, from the disassembly: the program each site binds, the function around it).
const PassSite kPassSites[] = {
    { 0x7170d, "screenshot" }, { 0x717c1, "screenshot" },
    { 0x740f8, "final output (outcompose)" },
    { 0x7eb74, "bloom" }, { 0x7ec93, "bloom" }, { 0x7edad, "bloom" }, { 0x7ef00, "bloom" },
    { 0x80dfd, "depth of field (focus)" },
    { 0x80f67, "depth of field" }, { 0x810cd, "depth of field" }, { 0x81224, "depth of field" },
    { 0x81380, "depth of field" }, { 0x81444, "depth of field" },
    { 0x82527, "light flares" },
    { 0x828b2, "sprites" },
    { 0x8597b, "lines" },
    { 0x85b5b, "water" },
    { 0x87255, "post (tonemap)" },
    { 0x87395, "exposure" }, { 0x874ef, "exposure" },
    { 0x87620, "disocclusion mask" },
    { 0x87b32, "composite" },
    { 0x87ebe, "light flares (visibility)" }, { 0x87ef0, "light flares (visibility)" },
    { 0x88933, "sky" },
    { 0x88b08, "smooth normals" },
    { 0x88bec, "disocclusion mask" },
    { 0x88d9f, "composite" },
    { 0x89003, "boundary" },
    { 0x896ea, "motion blur" },
    { 0x89a9a, "debug volume" },
    { 0x89c9d, "unlit 3D" },
    { 0x89dee, "boundary" },
    { 0x89f11, "debug lights" },
    { 0x8a120, "debug lights" },
    { 0x8b9b9, "debug lines" },
    { 0x8bdd3, "debug volume" },
    { 0x8bf69, "light: AO + sun" },
    { 0x8c35f, "light: lamps" },
    { 0x8ccb5, "light: denoise" },
    { 0x8cd3b, "light: diffuse" },
    { 0x8cff3, "fog: lamps" }, { 0x8d003, "fog: lamps" },
    { 0x8d247, "fog: sun" },
    { 0x8d329, "fog: denoise" },
    { 0x8d538, "particles" },
    { 0x8d679, "particles (GPU mesh)" },
    { 0x8d9f6, "ropes" }, { 0x8db1c, "ropes" }, { 0x8dbe7, "ropes" }, { 0x8dccf, "ropes" },
    { 0x8de40, "sky" },
    { 0x8df25, "screens" },
    { 0x8e3ec, "G-buffer: voxel shapes" }, { 0x8e6dc, "G-buffer: voxel shapes" },
    { 0x8f0d2, "water" },
    { 0x8f1e4, "lens dirt" }, { 0x8f25b, "lens dirt" },
    { 0x8f880, "lines" }, { 0x8f931, "lines" },
    { 0x8fb99, "outlines" }, { 0x8ff8b, "outlines" },
    { 0x901d4, "debug view" }, { 0x901e5, "debug view" }, { 0x90201, "debug view" },
    { 0x90826, "reflections" },
    { 0x909a1, "reflections: denoise" },
    { 0x90d66, "unlit 3D" },
    { 0x90e7f, "underwater" }, { 0x9117c, "underwater" }, { 0x91434, "underwater" },
    { 0x915f0, "rain wetness" }, { 0x9178b, "rain wetness" },
    { 0x9c570, "linearize depth" },
    { 0x9cb11, "motion blur" },
    { 0x9d646, "smooth normals" },
    { 0x9fbc8, "voxel textures: mips" },
    { 0xa2a61, "shadow volume: mips" },
    { 0xa5547, "TAA" }, { 0xa562e, "TAA" }, { 0xa574b, "TAA" },
    { 0x19c92b, "2D (engine)" }, { 0x19ce15, "2D (engine)" }, { 0x19d2da, "2D (engine)" }, { 0x19d55a, "2D (engine)" },
    { 0x19d663, "2D (engine)" }, { 0x19d763, "2D (engine)" }, { 0x19dc46, "2D (engine)" }, { 0x19dfad, "2D (engine)" },
    { 0x19e59a, "2D (engine)" }, { 0x19e710, "2D (engine)" }, { 0x1ac3ba, "2D (engine)" },
    { 0x468142, "scripts 2D (draw)" },
    { 0x4be206, "HUD blur" }, { 0x4be2c9, "HUD blur" },
    { 0x4befb9, "UI (scripts)" }, { 0x4bf4e0, "UI (scripts)" }, { 0x4c4dff, "UI (scripts)" }, { 0x4c5175, "UI (scripts)" },
    { 0x4c588a, "UI (scripts)" }, { 0x4c5d23, "UI (scripts)" }, { 0x4caf24, "UI (scripts)" }, { 0x4cd813, "UI (scripts)" },
    { 0x4cfefb, "UI (scripts)" }, { 0x4d07ff, "UI (scripts)" }, { 0x4d08b6, "UI (scripts)" }, { 0x4d0b4c, "UI (scripts)" },
    { 0x5b17ed, "renderer internal" }, { 0x5b44f3, "renderer internal" }, { 0x5b7f82, "renderer internal" },
    { 0x5b9991, "renderer internal" }, { 0x5b9a87, "renderer internal" },
    { 0x5e72ae, "2D (engine)" }, { 0x5e766e, "2D (engine)" },
};
bool g_passNames = false;               // Teardown 2.0.1's code: the sites above are named
uint32_t g_passCl = 0;                  // TRendererD3D12 -> the frame's command list (+e40, the set-program patterns)
uint32_t g_passInFrame = 0;             // ... -> "in a frame" (+cc: set by beginRender before the list's Reset,
                                        // cleared by endRender once the list is submitted)
BYTE* g_passBackend = nullptr;          // the renderer's TRendererD3D12 (Renderer +9c0, Hook_Render)
bool g_passInRender = false;            // between Renderer::render's start and return (the two region hooks)
constexpr int kPassCache = 512;         // call site -> pass, open addressing (main thread)
DWORD g_passCacheKey[kPassCache];
uint8_t g_passCacheLab[kPassCache];
struct PassSums
{
    int frames = 0, marks = 0, over = 0, unordered = 0, unbounded = 0;
    double ms[kPassLabels] = {};        // per pass, summed over the frames
};
PassSums g_passSum[3];                  // per eye: 0 = A, 1 = B, 2 = neither

using PFN_SetProgram = unsigned __int64(__fastcall*)(BYTE* backend, void* program);
PFN_SetProgram Real_SetGfxProgram = nullptr;
PFN_SetProgram Real_SetCsProgram = nullptr;
using PFN_PassRegion = unsigned __int64(__fastcall*)(BYTE* self);
PFN_PassRegion Real_VoxelTextures = nullptr;
PFN_PassRegion Real_ShadowVolume = nullptr;

int PassIntern(const char* name)
{
    for (int i = 0; i < g_passLabels; ++i)
        if (!strcmp(g_passLabel[i].name, name)) return i;
    if (g_passLabels >= kPassLabels) return kPassOther;
    snprintf(g_passLabel[g_passLabels].name, sizeof(g_passLabel[0].name), "%s", name);
    return g_passLabels++;
}

// Right after an indirect call: call [reg+190h / 198h] (FF 90+r disp32, 41 for r8-r15) or call reg (FF D0+r, 41).
bool PassSiteIsCall(const BYTE* ret)
{
    uint32_t d = 0;
    memcpy(&d, ret - 4, 4);
    const bool disp = (d == 0x190 || d == 0x198) && ret[-6] == 0xFF && (ret[-5] & 0xF8) == 0x90;
    const bool reg = ret[-2] == 0xFF && (ret[-1] & 0xF8) == 0xD0;
    return disp || reg;
}

// The pass of a call site (main thread).
int PassOfSite(void* ret)
{
    const BYTE* p = (const BYTE*)ret;
    if (p < g_base || p >= g_base + g_imageSize) return kPassOutside;
    const DWORD rva = (DWORD)(p - g_base);
    unsigned h = (rva * 2654435761u) >> 23;   // (9 bits)
    for (int i = 0; i < kPassCache; ++i, h = (h + 1) & (kPassCache - 1)) {
        if (g_passCacheKey[h] == rva) return g_passCacheLab[h];
        if (g_passCacheKey[h]) continue;
        int lab = -1;
        if (g_passNames)
            for (const PassSite& s : kPassSites)
                if (s.ret == rva) {
                    lab = PassIntern(s.name);
                    break;
                }
        if (lab < 0) {
            char name[24];
            snprintf(name, sizeof(name), "+%x", (unsigned)CallerRva(ret));
            lab = PassIntern(name);
        }
        g_passCacheKey[h] = rva;
        g_passCacheLab[h] = (uint8_t)lab;
        return lab;
    }
    return kPassOther;   // (512 sites seen)
}

// A pass starts here: a timestamp in the game's command list, unless the same pass goes on (main thread).
void PassMark(BYTE* backend, int label)
{
    GpuState& g = g_gpu;
    if (!backend || !g.passOk || !g.primed) return;
    GpuFrame& f = g.f[g.cur];
    if (f.used) return;                                   // (an older frame's timestamps, not read yet: untimed)
    if (f.pn && f.plabel[f.pn - 1] == label) return;      // (the same pass goes on)
    if (f.pn >= kPassMarks) {                             // (the frame's last pass takes the rest)
        f.pover = true;
        return;
    }
    if (!*(volatile const BYTE*)(backend + g_passInFrame)) return;   // (outside a frame: the list is closed)
    ID3D12GraphicsCommandList* cl = *(ID3D12GraphicsCommandList* const*)(backend + g_passCl);
    if (!cl) return;
    cl->EndQuery(g.passHeap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(g.cur * kPassMarks + f.pn));
    f.plabel[f.pn++] = (uint8_t)label;
}

unsigned __int64 __fastcall Hook_SetGfxProgram(BYTE* backend, void* program)
{
    if ((g_detail.load(std::memory_order_relaxed) & kDetailPasses) && OnMain())
        PassMark(backend, PassOfSite(_ReturnAddress()));
    return Real_SetGfxProgram(backend, program);
}

unsigned __int64 __fastcall Hook_SetCsProgram(BYTE* backend, void* program)
{
    if ((g_detail.load(std::memory_order_relaxed) & kDetailPasses) && OnMain())
        PassMark(backend, PassOfSite(_ReturnAddress()));
    return Real_SetCsProgram(backend, program);
}

// The two region hooks: only inside Renderer::render (the renderer of this frame's Hook_Render).
void PassRegion(int label)
{
    if ((g_detail.load(std::memory_order_relaxed) & kDetailPasses) && g_passInRender && OnMain())
        PassMark(g_passBackend, label);
}

unsigned __int64 __fastcall Hook_VoxelTextures(BYTE* renderer)
{
    PassRegion(kPassVoxels);
    const unsigned __int64 r = Real_VoxelTextures(renderer);
    PassRegion(kPassRender);
    return r;
}

unsigned __int64 __fastcall Hook_ShadowVolume(BYTE* volume)
{
    PassRegion(kPassShadow);
    const unsigned __int64 r = Real_ShadowVolume(volume);
    PassRegion(kPassRender);
    return r;
}

// GpuInit (perfDetail 8 hooked): the pass timestamps' heap and readback buffer (kPassMarks per frame slot), per
// slot a list that copies its timestamps out, and one list, submitted once before any pass timestamp, that writes
// all of them (a slot's copy never reads a timestamp never written). Recorded once from the slots' allocators.
bool PassInit(ID3D12Device* dev)
{
    GpuState& g = g_gpu;
    const D3D12_QUERY_HEAP_DESC hd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, (UINT)(kGpuRing * kPassMarks), 0 };
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (UINT64)kGpuRing * kPassMarks * sizeof(UINT64);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(dev->CreateQueryHeap(&hd, IID_PPV_ARGS(&g.passHeap))) &&
              SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g.passReadback))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.f[0].alloc, nullptr,
                                               IID_PPV_ARGS(&g.passPrime)));
    if (ok) {
        for (UINT i = 0; i < (UINT)(kGpuRing * kPassMarks); ++i)
            g.passPrime->EndQuery(g.passHeap, D3D12_QUERY_TYPE_TIMESTAMP, i);
        ok = SUCCEEDED(g.passPrime->Close());
    }
    for (int s = 0; ok && s < kGpuRing; ++s) {
        GpuFrame& f = g.f[s];
        ok = SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.alloc, nullptr, IID_PPV_ARGS(&f.passResolve)));
        if (!ok) break;
        const UINT base = (UINT)(s * kPassMarks);
        f.passResolve->ResolveQueryData(g.passHeap, D3D12_QUERY_TYPE_TIMESTAMP, base, (UINT)kPassMarks, g.passReadback,
                                        (UINT64)base * sizeof(UINT64));
        ok = SUCCEEDED(f.passResolve->Close());
    }
    // (what was made stays: a few small objects, once)
    if (!ok) Log("Perf: GPU time per pass not set up (a query heap, buffer or command list could not be made)");
    return ok;
}

// A frame's pass timestamps -> its eye's sums. t: its submissions' timestamps (4), p: its pass timestamps.
void PassAccount(const GpuFrame& f, const UINT64* t, const UINT64* p)
{
    GpuState& g = g_gpu;
    PassSums& s = g_passSum[f.eye];
    // (the game's submission - not made in the Present - that holds the first mark)
    int sub = -1;
    for (int i = 0; i + 1 < f.n; i += 2)
        if (!f.ours[i / 2] && t[i] <= p[0] && p[0] <= t[i + 1]) {
            sub = i;
            break;
        }
    if (sub < 0 || (double)(t[sub + 1] - t[sub]) / g.ticksPerMs > 1000.0) {
        ++s.unbounded;
        return;
    }
    UINT64 prev = t[sub];
    int label = kPassStart;
    for (int k = 0; k <= f.pn; ++k) {
        UINT64 at = k < f.pn ? p[k] : t[sub + 1];
        if (at < prev) {   // (a timestamp before the one before it: that pass counts 0)
            ++s.unordered;
            at = prev;
        }
        s.ms[label] += (double)(at - prev) / g.ticksPerMs;
        prev = at;
        if (k < f.pn) label = f.plabel[k];
    }
    ++s.frames;
    s.marks += f.pn;
    if (f.pover) ++s.over;
}

// GpuRead: a frame read back (slot: its ring slot, t: its submissions' timestamps, mapped).
void PassRead(const GpuFrame& f, int slot, const UINT64* t)
{
    GpuState& g = g_gpu;
    if (!g.passOk || f.pn <= 0 || !f.presolved || f.eye < 0 || f.eye > 2) return;
    const UINT base = (UINT)(slot * kPassMarks);
    const D3D12_RANGE r = { (SIZE_T)base * sizeof(UINT64), (SIZE_T)(base + f.pn) * sizeof(UINT64) };
    void* p = nullptr;
    if (FAILED(g.passReadback->Map(0, &r, &p)) || !p) return;
    PassAccount(f, t, (const UINT64*)p + base);
    const D3D12_RANGE none = { 0, 0 };
    g.passReadback->Unmap(0, &none);
}

// InstallDetail (perfDetail 8 asked for at start): the hooks, the offsets their patterns hold, the names.
bool PassInstall(const Targets& t)
{
    auto rva = [](const BYTE* p) { return p ? (unsigned long long)(p - g_base) : 0ull; };
    if (!t.setGfxProgram || !t.setCsProgram || !t.beginRender) {
        Log("Perf: GPU time per pass NOT available (set-program +%llx / +%llx, beginRender +%llx: not recognised)",
            rva(t.setGfxProgram), rva(t.setCsProgram), rva(t.beginRender));
        return false;
    }
    // (engine.cpp's patterns: mov rcx,[rcx+0E40h] at +4b / +46 of the set-program methods, mov byte [rcx+0CCh],1 at
    // +15 of beginRender)
    uint32_t clGfx = 0, clCs = 0, inFrame = 0;
    memcpy(&clGfx, t.setGfxProgram + 75, 4);
    memcpy(&clCs, t.setCsProgram + 70, 4);
    memcpy(&inFrame, t.beginRender + 23, 4);
    if (clGfx != clCs || clGfx >= 0x10000 || inFrame >= 0x10000) {
        Log("Perf: GPU time per pass NOT available (command list +%x / +%x, in-a-frame flag +%x)", clGfx, clCs, inFrame);
        return false;
    }
    g_passCl = clGfx;
    g_passInFrame = inFrame;
    // (Real_ checked too: a hook another module made first gives MH_ERROR_ALREADY_CREATED, taken as success; a hook
    // installed here passes straight through while bit 8 is off)
    const bool gfx = HookFunction(t.setGfxProgram, (void*)Hook_SetGfxProgram, (void**)&Real_SetGfxProgram) && Real_SetGfxProgram;
    const bool cs = HookFunction(t.setCsProgram, (void*)Hook_SetCsProgram, (void**)&Real_SetCsProgram) && Real_SetCsProgram;
    if (!gfx || !cs) {
        Log("Perf: GPU time per pass NOT available (set-program +%llx %s, +%llx %s)", rva(t.setGfxProgram),
            gfx ? "hooked" : "NOT hooked", rva(t.setCsProgram), cs ? "hooked" : "NOT hooked");
        return false;
    }
    const bool vox = t.voxelTextures && HookFunction(t.voxelTextures, (void*)Hook_VoxelTextures, (void**)&Real_VoxelTextures) &&
                     Real_VoxelTextures;
    const bool shadow = t.shadowVolume && HookFunction(t.shadowVolume, (void*)Hook_ShadowVolume, (void**)&Real_ShadowVolume) &&
                        Real_ShadowVolume;
    // Teardown 2.0.1's names: its set-program methods where 2.0.1 has them, and every named site right after a call
    g_passNames = rva(t.setGfxProgram) == 0x5b3890 && rva(t.setCsProgram) == 0x5b12e0;
    for (const PassSite& s : kPassSites)
        if (g_passNames && (s.ret < 8 || s.ret >= g_imageSize || !PassSiteIsCall(g_base + s.ret))) g_passNames = false;
    // (the fixed passes, in the enum's order)
    g_passLabels = 0;
    PassIntern("frame start");
    PassIntern("render setup");
    PassIntern("after render (HUD and UI)");
    PassIntern("voxel textures: upload");
    PassIntern("shadow volume");
    PassIntern("other");
    PassIntern("outside teardown.exe");
    Log("Perf: GPU time per pass (perfDetail 8): set-program +%llx / +%llx hooked, voxel textures +%llx %s, shadow volume "
        "+%llx %s; a timestamp in the game's command list (+%x, in a frame +%x) where the pass changes, at most %d per "
        "frame; passes named by %s - line \"Perf GPU passes\" after each \"Stereo frames\" line",
        rva(t.setGfxProgram), rva(t.setCsProgram), rva(t.voxelTextures), vox ? "hooked" : "NOT hooked", rva(t.shadowVolume),
        shadow ? "hooked" : "NOT hooked", g_passCl, g_passInFrame, kPassMarks,
        g_passNames ? "call site (Teardown 2.0.1)" : "calling function (+rva: not Teardown 2.0.1's code)");
    return true;
}

// LogInstrument: this window's passes (bit 8 on), then the sums start again.
void PassLog(int on)
{
    if ((on & kDetailPasses) && g_gpu.passOk) {
        const PassSums& a = g_passSum[0];
        const PassSums& b = g_passSum[1];
        const double na = std::max(1, a.frames), nb = std::max(1, b.frames);
        std::vector<int> v;
        double ta = 0, tb = 0;
        for (int k = 0; k < g_passLabels; ++k)
            if (a.ms[k] > 0 || b.ms[k] > 0) {
                v.push_back(k);
                ta += a.ms[k];
                tb += b.ms[k];
            }
        std::sort(v.begin(), v.end(), [&](int x, int y) { return a.ms[x] / na + b.ms[x] / nb > a.ms[y] / na + b.ms[y] / nb; });
        std::string s = "Perf GPU passes (ms per frame of each eye; a timestamp in the game's command list where the pass "
                        "changes)";
        AppendF(s, " A: %d frames, %.1f marks, total %.2f | B: %d frames, %.1f marks, total %.2f | pass=A/B:", a.frames,
                a.marks / na, ta / na, b.frames, b.marks / nb, tb / nb);
        for (size_t i = 0; i < v.size(); ++i)
            AppendF(s, "%s %s=%.3f/%.3f", i ? "," : "", g_passLabel[v[i]].name, a.ms[v[i]] / na, b.ms[v[i]] / nb);
        if (a.over || b.over)
            AppendF(s, "; frames over %d marks (their last pass took the rest) A %d, B %d", kPassMarks, a.over, b.over);
        if (a.unordered || b.unordered) AppendF(s, "; timestamps out of order A %d, B %d", a.unordered, b.unordered);
        if (a.unbounded || b.unbounded)
            AppendF(s, "; frames whose game submission was not found A %d, B %d", a.unbounded, b.unbounded);
        LogSplit(s, "Perf GPU passes (cont.): ");
    }
    for (PassSums& x : g_passSum) x = PassSums{};
}


bool GpuInit(ID3D12CommandQueue* q)
{
    if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        Log("Perf: GPU timestamps not set up (the swap chain's queue is not a direct queue)");
        return false;
    }
    UINT64 freq = 0;
    ID3D12Device* dev = nullptr;
    if (FAILED(q->GetTimestampFrequency(&freq)) || !freq || FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) {
        Log("Perf: GPU timestamps not set up (no timestamp frequency or device)");
        return false;
    }
    GpuState& g = g_gpu;
    const D3D12_QUERY_HEAP_DESC hd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kGpuRing * kGpuMarks, 0 };
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (UINT64)kGpuRing * kGpuMarks * sizeof(UINT64);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool ok = SUCCEEDED(dev->CreateQueryHeap(&hd, IID_PPV_ARGS(&g.heap))) &&
              SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g.readback))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    // (every list is recorded once, from its frame's allocator, never reset: a frame's lists run again
    // only after that frame was read, kGpuRing frames later)
    for (int s = 0; ok && s < kGpuRing; ++s) {
        GpuFrame& f = g.f[s];
        const UINT base = (UINT)(s * kGpuMarks);
        ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)));
        for (int i = 0; ok && i < kGpuMarks; ++i) {
            ok = SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.alloc, nullptr, IID_PPV_ARGS(&f.mark[i])));
            if (!ok) break;
            f.mark[i]->EndQuery(g.heap, D3D12_QUERY_TYPE_TIMESTAMP, base + i);
            ok = SUCCEEDED(f.mark[i]->Close());
        }
        for (int k = 0; ok && k < kGpuMarks / 2; ++k) {
            ok = SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.alloc, nullptr, IID_PPV_ARGS(&f.resolve[k])));
            if (!ok) break;
            f.resolve[k]->ResolveQueryData(g.heap, D3D12_QUERY_TYPE_TIMESTAMP, base, (UINT)(2 * (k + 1)), g.readback,
                                           (UINT64)base * sizeof(UINT64));
            ok = SUCCEEDED(f.resolve[k]->Close());
        }
    }
    if (ok && (g_detailHooks & kDetailPasses)) g.passOk = PassInit(dev);   // (perfDetail 8)
    dev->Release();
    if (!ok) {
        // (what was made stays: a few small objects, once)
        Log("Perf: GPU timestamps not set up (a query heap, buffer, fence or command list could not be made)");
        return false;
    }
    g.ticksPerMs = (double)freq / 1000.0;
    g.queue = q;
    Log("Perf: GPU timestamps on the game's queue (%.1f MHz): its busy and idle time per frame on the \"Perf GPU\" line",
        (double)freq / 1e6);
    return true;
}

// A frame's timestamps (start, end of each submission) -> its eye's sums.
void GpuAccount(const GpuFrame& f, const UINT64* t)
{
    GpuState& g = g_gpu;
    GpuSums& s = g_gpuSum[f.eye];
    UINT64 prev = f.gapBefore ? 0 : g.lastEnd;
    double game = 0, ours = 0, inside = 0, before = -1;
    for (int i = 0; i + 1 < f.n; i += 2) {
        const UINT64 a = t[i], b = t[i + 1];
        if (b < a) {   // (not plausible: the frame is not counted)
            g.lastEnd = 0;
            return;
        }
        if (prev && a > prev && (double)(a - prev) / g.ticksPerMs > 1000.0) prev = 0;   // (a pause: gap unknown)
        const UINT64 start = prev > a ? prev : a;
        const double gap = prev && a > prev ? (double)(a - prev) / g.ticksPerMs : 0;
        const double busy = b > start ? (double)(b - start) / g.ticksPerMs : 0;
        (f.ours[i / 2] ? ours : game) += busy;
        if (i == 0) before = prev ? gap : -1;
        else inside += gap;
        prev = std::max(prev, b);
    }
    g.lastEnd = prev;
    ++s.frames;
    s.game += game;
    s.ours += ours;
    s.idleInside += inside;
    if (before >= 0) {
        s.idleBefore += before;
        ++s.idleKnown;
    }
    s.longest = std::max(s.longest, game + ours);
    s.submissions += f.n / 2;
    if (f.full) ++s.unmarked;
}

// Every Present: the frames whose timestamps arrived (our fence reached), oldest first.
void GpuRead()
{
    GpuState& g = g_gpu;
    if (!g.f[g.next].used || !g.f[g.next].ready) return;
    const UINT64 done = g.fence->GetCompletedValue();
    if (done == UINT64_MAX) {   // (the device was removed)
        g.ok = false;
        Log("Perf: GPU timestamps stopped (device removed)");
        return;
    }
    while (g.f[g.next].used && g.f[g.next].ready && g.f[g.next].ready <= done) {
        GpuFrame& f = g.f[g.next];
        const UINT base = (UINT)(g.next * kGpuMarks);
        const D3D12_RANGE r = { (SIZE_T)base * sizeof(UINT64), (SIZE_T)(base + f.n) * sizeof(UINT64) };
        void* p = nullptr;
        if (SUCCEEDED(g.readback->Map(0, &r, &p)) && p) {
            GpuAccount(f, (const UINT64*)p + base);
            PassRead(f, g.next, (const UINT64*)p + base);   // (perfDetail 8)
            const D3D12_RANGE none = { 0, 0 };
            g.readback->Unmap(0, &none);
        }
        f.used = f.full = false;
        f.ready = 0;
        f.n = 0;
        f.pn = 0;
        f.pover = f.presolved = false;
        g.next = (g.next + 1) % kGpuRing;
    }
}

// ---------------------------------------------------------------------------------------------------
// The details' sums, the control file, set-up and the lines
// ---------------------------------------------------------------------------------------------------
struct DetailSums
{
    int frames = 0;
    double blocking = 0, blockingMs = 0, hist[kHist] = {}, state[4] = {};
    WaitBy waits[kWaitEye];             // Job::wait by caller: ms, waits over 2 us
    int waitsN = 0;
    WaitBy fence[kFenceBy];
    int fenceN = 0;
    double offFenceMs = 0, offFenceN = 0;
    double upBytes = 0, upSubmits = 0, upForced = 0;
    double frameMax[kJobTypes + 1] = {};  // each class's longest run per frame (TSC), summed over frames...
    int frameMaxN[kJobTypes + 1] = {};    // ... in which it ran
};
DetailSums g_det[3];                    // per eye: 0 = A, 1 = B, 2 = neither

// FrameEnd (vr.cpp, every Present): this frame's details to its eye.
void DetailFrameEnd(int engineEye)
{
    const int e = engineEye == 0 ? 0 : engineEye == 1 ? 1 : 2;
    g_gpuEye = e;   // (GpuFrameEnd, later in this Present)
    const int on = g_detail.load(std::memory_order_relaxed);
    if (!on) return;
    DetailSums& s = g_det[e];
    ++s.frames;
    if (on & kDetailWaits) {
        DetailFrame& f = g_detFrame;
        s.blocking += f.blocking;
        s.blockingMs += f.blockingMs;
        for (int b = 0; b < kHist; ++b) s.hist[b] += f.hist[b];
        for (int k = 0; k < 4; ++k) s.state[k] += f.state[k];
        for (int i = 0; i < f.fenceN; ++i) AddBy(s.fence, s.fenceN, kFenceBy, f.fence[i].rva, f.fence[i].ms, f.fence[i].n);
        // (the job waits by caller of this frame: Hook_JobWait's table, cleared right after)
        for (int i = 0; i < g_waitFrameN; ++i)
            AddBy(s.waits, s.waitsN, (int)kWaitEye, g_waitFrame[i].rva, g_waitFrame[i].ms, g_waitFrame[i].n);
        s.offFenceMs += (double)g_offFenceUs.exchange(0, std::memory_order_relaxed) / 1000.0;
        s.offFenceN += g_offFenceN.exchange(0, std::memory_order_relaxed);
        s.upBytes += (double)g_upBytes.exchange(0, std::memory_order_relaxed);
        s.upSubmits += g_upSubmits.exchange(0, std::memory_order_relaxed);
        s.upForced += g_upForced.exchange(0, std::memory_order_relaxed);
        f = DetailFrame{};
    }
    if (on & kDetailJobs)
        for (int k = 0; k <= kJobTypes; ++k)
            if (g_jobFrameMax[k].load(std::memory_order_relaxed)) {
                s.frameMax[k] += (double)g_jobFrameMax[k].exchange(0, std::memory_order_relaxed);
                ++s.frameMaxN[k];
            }
}

// TeardownVR_perf.txt (frameb.h's control file, next to the game): "perfDetail=N" (0-15). Read here, at each
// statistics line and only when the file changed, so that every window is measured one way - also when
// the frame-B module (which polls the file every second) is off.
int FileDetail()
{
    static FILETIME s_time = {};
    static DWORD s_size = 0;
    static int s_value = -1;
    static const std::wstring path = ModDirectory() + L"TeardownVR_perf.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        s_time = {};
        s_size = 0;
        return s_value = -1;
    }
    if (CompareFileTime(&fa.ftLastWriteTime, &s_time) == 0 && fa.nFileSizeLow == s_size) return s_value;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s_value;   // (being written: read at the next line)
    char buf[512] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    s_time = fa.ftLastWriteTime;
    s_size = fa.nFileSizeLow;
    s_value = -1;
    for (const char* p = buf; (p = strstr(p, "perfDetail")) != nullptr; p += 10) {
        const char* q = p + 10;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q >= '0' && *q <= '9') {
            int v = 0;
            while (*q >= '0' && *q <= '9' && v < 100) v = v * 10 + (*q++ - '0');
            s_value = std::min(v, 15);
            break;
        }
    }
    return s_value;
}

// The details asked for: the control file's "perfDetail=N" if it has one, else the ini's.
int DetailWanted()
{
    const int file = FileDetail();
    const int want = std::clamp(file >= 0 ? file : Cfg().perfDetail, 0, 15);
    return (want & kDetailPasses) ? (want | kDetailGpu) : want;   // (8's timestamps are read back with 4's)
}

// InstallHooks (perfTrace >= 1, first Present): the hooks of the bits asked for at start.
void InstallDetail(const Targets& t)
{
    const int want = DetailWanted();
    if (!want) return;
    // (waits: perfTrace 1's own hooks, plus the uploader's; GPU: set up at the next Present, GpuQueue)
    int hooks = want & (kDetailWaits | kDetailGpu);
    // (Real_ checked too: a hook another module made first gives MH_ERROR_ALREADY_CREATED, taken as success)
    const bool up = (want & kDetailWaits) && t.uploadSubmit &&
                    HookFunction(t.uploadSubmit, (void*)Hook_UpSubmit, (void**)&Real_UpSubmit) && Real_UpSubmit;
    const bool run = (want & kDetailJobs) && t.jobRun && HookFunction(t.jobRun, (void*)Hook_JobRun, (void**)&Real_JobRun) &&
                     Real_JobRun;
    if (run) hooks |= kDetailJobs;
    if ((want & kDetailPasses) && PassInstall(t)) hooks |= kDetailPasses;   // (GPU time per pass)
    g_tscLast = __rdtsc();
    g_msLast = ProfNow();
    g_detailHooks = hooks;
    g_detail.store(hooks, std::memory_order_relaxed);
    auto rva = [](BYTE* p) { return p ? (unsigned long long)(p - g_base) : 0ull; };
    Log("Perf: details on (perfDetail=%d): blocking Job::wait histogram, GPU fence waits by caller %s, async uploader "
        "submit +%llx %s; Job::run +%llx %s; GPU timestamps %s - lines \"Perf waits / jobs / GPU\" after each "
        "\"Stereo frames\" line", want, (want & kDetailWaits) ? "on" : "off", rva(t.uploadSubmit),
        up ? "hooked" : (want & kDetailWaits) ? "NOT hooked" : "off", rva(t.jobRun),
        run ? "hooked (every job timed by class)" : (want & kDetailJobs) ? "NOT hooked" : "off",
        (want & kDetailGpu) ? "at the next Present" : "off");
}

// The labels of the engine's GPU fence wait callers (Teardown 2.0.1 RVAs).
const char* FenceCaller(DWORD rva)
{
    switch (rva) {
    case 0x5b35a0: return " beginFrame";
    case 0x5ae070: return " uploader";
    case 0x5ba2b0: return " waitIdle";
    case 0x5bbcd0: return " resize";
    }
    return "";
}

// LogDetails (after each "Stereo frames" line, perfTrace >= 1): this window's details, then the control
// file's setting for the next window.
void LogInstrument()
{
    if (!g_detailHooks) return;
    const int on = g_detail.load(std::memory_order_relaxed);
    const char* eyeName[2] = { "A", "B" };
    if (on & kDetailWaits) {
        std::string s = "Perf waits (per frame)";
        for (int e = 0; e < 2; ++e) {
            DetailSums& x = g_det[e];
            const double n = std::max(1, x.frames);
            AppendF(s, "%s %s: Job::wait %.1f blocking %.2f ms (", e ? " |" : "", eyeName[e], x.blocking / n, x.blockingMs / n);
            for (int b = 0; b < kHist; ++b) AppendF(s, "%s%s %.2f", b ? ", " : "", kHistName[b], x.hist[b] / n);
            s += "; job then";
            for (int k = 0; k < 4; ++k) AppendF(s, "%s %s %.2f", k ? "," : "", kStateName[k], x.state[k] / n);
            s += ")";
            std::sort(x.waits, x.waits + x.waitsN, [](const WaitBy& a, const WaitBy& b) { return a.ms > b.ms; });
            for (int i = 0; i < x.waitsN && i < 6; ++i) {
                AppendWait(s, i ? " " : ", by caller ms/waits ", x.waits[i], x.waits[i].ms / n, "%.2f");
                AppendF(s, "/%.2f", x.waits[i].n / n);
            }
            double fence = 0;
            for (int i = 0; i < x.fenceN; ++i) fence += x.fence[i].ms;
            AppendF(s, "; GPU fence %.2f ms", fence / n);
            std::sort(x.fence, x.fence + x.fenceN, [](const WaitBy& a, const WaitBy& b) { return a.ms > b.ms; });
            for (int i = 0; i < x.fenceN; ++i) {
                if (x.fence[i].rva) AppendF(s, " +%x%s", x.fence[i].rva, FenceCaller(x.fence[i].rva));
                else s += " +other";
                AppendF(s, " %.2f/%.2f", x.fence[i].ms / n, x.fence[i].n / n);
            }
            AppendF(s, ", other threads %.2f/%.2f; uploads %.3f MB/%.2f submits (%.2f forced)", x.offFenceMs / n,
                    x.offFenceN / n, x.upBytes / n / 1048576.0, x.upSubmits / n, x.upForced / n);
        }
        LogSplit(s, "Perf waits (cont.): ");
    }
    if (on & kDetailJobs) {
        // (TSC -> ms over this window, by QPC)
        const uint64_t tsc = __rdtsc();
        const double now = ProfNow();
        if (now - g_msLast > 100) {
            g_tscPerMs = (double)(tsc - g_tscLast) / (now - g_msLast);
            g_tscLast = tsc;
            g_msLast = now;
        }
        // The window just ended. (A run that read the old window just before the flip may still add to it
        // after the reset below: it then counts in the window after next - one run per thread at most.)
        const int old = g_jobGen.load(std::memory_order_relaxed) & 1;
        g_jobGen.store(old ^ 1, std::memory_order_relaxed);
        static JobCell sum[2][kJobTypes + 1];
        for (auto& row : sum)
            for (JobCell& c : row) c = JobCell{};
        const int threads = std::min(g_jobAccN.load(std::memory_order_relaxed), kJobThreads);
        for (int i = 0; i < threads; ++i) {
            JobAcc* a = g_jobAcc[i].load(std::memory_order_acquire);
            if (!a) continue;
            for (int e = 0; e < 2; ++e)
                for (int k = 0; k <= kJobTypes; ++k) {
                    JobCell& c = a->c[old][e][k];
                    sum[e][k].ticks += c.ticks;
                    sum[e][k].n += c.n;
                    sum[e][k].max = std::max(sum[e][k].max, c.max);
                    c = JobCell{};
                }
        }
        const double tick = g_tscPerMs > 0 ? 1.0 / g_tscPerMs : 0;   // ms per TSC tick
        std::string s = "Perf jobs (per frame; each run's own time, all threads)";
        for (int e = 0; e < 2; ++e) {
            const DetailSums& x = g_det[e];
            const double n = std::max(1, x.frames);
            double total = 0, jobs = 0;
            int longest = -1;
            std::vector<int> v;
            for (int k = 0; k <= kJobTypes; ++k) {
                if (!sum[e][k].n) continue;
                v.push_back(k);
                total += (double)sum[e][k].ticks;
                jobs += sum[e][k].n;
                if (longest < 0 || sum[e][k].max > sum[e][longest].max) longest = k;
            }
            std::sort(v.begin(), v.end(), [&](int a, int b) { return sum[e][a].ticks > sum[e][b].ticks; });
            AppendF(s, "%s %s: %.1f jobs %.2f ms (blocking waits %.2f ms)", e ? " |" : "", eyeName[e], jobs / n, total * tick / n,
                    x.blockingMs / n);
            if (longest >= 0) AppendF(s, ", longest run %s %.2f ms", JobLabel(longest).c_str(), (double)sum[e][longest].max * tick);
            for (size_t i = 0; i < v.size() && i < 8; ++i) {
                const int k = v[i];
                const JobCell& c = sum[e][k];
                AppendF(s, "%s%s %.2f ms/%.1f (mean %.0f us, longest %.2f, longest per frame %.2f)", i ? ", " : "; by class ",
                        JobLabel(k).c_str(), (double)c.ticks * tick / n, c.n / n, (double)c.ticks * tick * 1000.0 / c.n,
                        (double)c.max * tick, x.frameMaxN[k] ? x.frameMax[k] * tick / x.frameMaxN[k] : 0.0);
            }
        }
        LogSplit(s, "Perf jobs (cont.): ");
    }
    if (on & kDetailGpu) {
        std::string s = "Perf GPU (per frame; timestamps around each submission on the game's queue)";
        double busy[2] = {}, all = 0, idle = 0;
        for (int e = 0; e < 2; ++e) {
            const GpuSums& g = g_gpuSum[e];
            const double n = std::max(1, g.frames);
            busy[e] = (g.game + g.ours) / n;
            all += g.game + g.ours;
            idle += g.idleBefore + g.idleInside;
            AppendF(s, "%s %s: %d frames, busy %.2f ms (game %.2f, ours %.2f), longest %.2f, idle before %.2f, idle inside "
                       "%.2f, %.1f submissions", e ? " |" : "", eyeName[e], g.frames, busy[e], g.game / n, g.ours / n,
                    g.longest, g.idleKnown ? g.idleBefore / g.idleKnown : 0.0, g.idleInside / n, g.submissions / n);
            if (g.unmarked) AppendF(s, " (%d frames with a submission not timed)", g.unmarked);
        }
        AppendF(s, " | pair: busy %.2f ms (A + B), the GPU busy %.0f%% of its time", busy[0] + busy[1],
                all + idle > 0 ? all / (all + idle) * 100.0 : 0.0);
        if (!g_gpu.ok) s += g_gpu.tried ? " (timestamps stopped)" : " (timestamps not set up yet)";
        Log("%s", s.c_str());
    }
    PassLog(on);   // (perfDetail 8: "Perf GPU passes"; its sums start again)
    for (DetailSums& d : g_det) d = DetailSums{};
    for (GpuSums& g : g_gpuSum) g = GpuSums{};
    // (the control file: the next window's setting, among the bits set up at start)
    const int want = DetailWanted() & g_detailHooks;
    if (want != on) {
        Log("Perf: details now perfDetail=%d (control file TeardownVR_perf.txt or ini; set up at start: %d)", want, g_detailHooks);
        g_detFrame = DetailFrame{};
        g_detail.store(want, std::memory_order_relaxed);
    }
}

void SafeName(const char* p, char* out, size_t n)
{
    // (an engine string of its .rdata; copied with a bound anyway)
    size_t i = 0;
    for (; p && i + 1 < n && p[i]; ++i) out[i] = p[i];
    out[i] = 0;
}

// ---------------------------------------------------------------------------------------------------
// Lazy beginFrame ([Performance] lazyBeginFrame, perf.h)
// ---------------------------------------------------------------------------------------------------
// TRendererD3D12 (Teardown 2.0.1: vtable +a81d80, the 74 virtuals of TRendererContext). Its methods are
// reached only through that vtable (teardown.exe calls none of them directly but the destructor, +248),
// and they alone touch the frame context beginRender (slot +10) prepares after its wait: the command
// allocator and list (+e40), the shader-visible descriptor ring (+f88), the sync uploader (+10d8), the
// barrier batcher (+1130), the deferred texture list (+13d0, under +1378) and the context's resource
// deleter. Game code releases GPU resources into the pending deleter (+fa0, which endRender hands to the
// frame's context) and uploads through the async uploader (+fc0: its own queue and fence, submitted by
// endRender): neither depends on beginRender having run. So once frame A's beginRender is held back, the
// first call of any other method - but the few below that provably touch no frame context - runs it
// first: every command is then recorded as before, in the same order, by the same thread; only the CPU
// waits later (or not at all, the GPU having finished meanwhile).
constexpr int kRendSlots = 74;
constexpr int kSlotBegin = 2;           // +10 beginRender
constexpr int kStubSize = 32;
// The stubs' data page. The generated code reads pending at +0 and orig at +8.
struct LazyPage
{
    volatile char pending;              // a frame's beginRender is held back
    char pad[7];
    void* orig[kRendSlots];             // the engine's methods
    RUNTIME_FUNCTION fn;                // the stubs' common part (exception unwinding through it)
    BYTE unwind[8];                     // its UNWIND_INFO: A8h bytes of stack, nothing to restore
};
static_assert(offsetof(LazyPage, orig) == 8, "the stubs read orig at +8");
using PFN_Method = void(__fastcall*)(BYTE* self);
LazyPage* g_lz = nullptr;               // nullptr: not installed
void** g_lzVt = nullptr;
int (*g_lzKind)() = nullptr;            // engine.cpp: the frame starting (1 = a stereo pair's frame A, 2 = its frame B)
uint32_t g_lzOwner = 0, g_lzCtxIdx = 0, g_lzCtxs = 0, g_lzFence = 0;   // renderer offsets (beginRender's pattern)
BYTE* g_lzR = nullptr;                  // the renderer whose beginRender is held back
uint64_t g_lzOwnerVal = 0;              // its frame owner then (+f70: the thread of the last beginRender, the main one)
DWORD g_lzTid = 0;                      // the main thread (the one calling beginRender)
double g_lzSkipAt = 0;
SRWLOCK g_lzLock = SRWLOCK_INIT;
thread_local bool t_lzInBegin = false;  // this thread runs the held-back beginRender (its own method calls pass)
bool g_lzOff = false;                   // an unexpected case was met: off until the game restarts
int g_lzMode = -1;                      // the mode last used (logged when it changes)
struct LazyStats
{
    int framesA = 0;                    // stereo frames A seen
    int heldA = 0, heldB = 0;           // beginRender held back in frames A / B
    int busyStart = 0;                  // ... while the GPU was still busy at the frame start (it would have waited)
    int busyUse = 0;                    // ... still busy at the first use (the wait that remains)
    double gapMs = 0, beginMs = 0, beginMax = 0;
    int plain = 0, plainBusy = 0;       // frames A whose beginRender ran at their start (mode 0)
    double plainMs = 0, plainMax = 0;
    struct Use { int slot; DWORD rva; int n; } use[8];   // first uses: method, calling function
    int uses = 0;
};
LazyStats g_lzSt;

// beginRender's own test: the frame context's fence value (contexts +ef8, 30h each, index +da8, value at
// +8) not reached yet by the fence (+e58, ID3D12Fence::GetCompletedValue): it then waits.
bool LazyGpuBusy(BYTE* r)
{
    const BYTE* ctx = *(BYTE* const*)(r + g_lzCtxs) + (size_t)*(const int*)(r + g_lzCtxIdx) * 0x30;
    ID3D12Fence* fence = *(ID3D12Fence* const*)(r + g_lzFence);
    return fence && fence->GetCompletedValue() < *(const UINT64*)(ctx + 8);
}

int LazyMode()
{
    const int file = FrameB::FileLazyBegin();
    const int m = g_lzOff ? 0 : std::clamp(file >= 0 ? file : Cfg().lazyBeginFrame, 0, 2);
    if (m != g_lzMode) {
        Log("Perf: lazy beginFrame %d%s", m,
            m == 1 ? " - a stereo pair's frame A starts its update without waiting for the GPU; beginRender runs at "
                     "its first renderer use"
            : m == 2 ? " - frames A and B (test)" : " (off: beginRender at the frame start, as the game does)");
        g_lzMode = m;
    }
    return m;
}

// Runs the held-back beginRender (normally on the main thread, at a renderer method's first use).
// why: nullptr then, else the unexpected place it is run from.
void LazyRun(int slot, void* ret, const char* why)
{
    const bool mainThread = GetCurrentThreadId() == g_lzTid;
    bool ran = false, busy = false;
    double t0 = 0, ms = 0;
    AcquireSRWLockExclusive(&g_lzLock);
    if (g_lz->pending) {
        busy = LazyGpuBusy(g_lzR);
        t0 = ProfNow();
        t_lzInBegin = true;
        ((PFN_Method)g_lz->orig[kSlotBegin])(g_lzR);
        t_lzInBegin = false;
        // (on another thread: the main thread stays the frame's owner, as beginRender at the frame start
        // made it - a texture another thread creates then still goes to the deferred list, slot +e8)
        if (!mainThread) *(uint64_t*)(g_lzR + g_lzOwner) = g_lzOwnerVal;
        g_lz->pending = 0;
        ms = ProfNow() - t0;
        ran = true;
    }
    ReleaseSRWLockExclusive(&g_lzLock);
    if (!ran) return;
    if (!mainThread || why) {
        // Never expected (only the main thread uses the renderer, and endRender - guarded - comes before
        // the next frame): run where met, never later than the game would have waited, and off for the session.
        g_lzOff = true;
        Log("Perf: lazy beginFrame: the held-back beginRender ran %s (method +%x, from +%llx, %s thread) - lazy "
            "beginFrame OFF until the game restarts", why ? why : "at a renderer use", slot * 8,
            ret ? (unsigned long long)((BYTE*)ret - g_base) : 0ull, mainThread ? "main" : "another");
        return;
    }
    LazyStats& s = g_lzSt;
    s.gapMs += t0 - g_lzSkipAt;
    s.beginMs += ms;
    s.beginMax = std::max(s.beginMax, ms);
    if (busy) ++s.busyUse;
    const DWORD by = CallerRva(ret);
    int i = 0;
    while (i < s.uses && (s.use[i].slot != slot || s.use[i].rva != by)) ++i;
    if (i == s.uses && s.uses < 8) s.use[s.uses++] = { slot, by, 0 };
    if (i < s.uses) ++s.use[i].n;
}

// Every guarded method's stub calls this while a beginRender is held back (the method's slot, its
// caller's return address, the renderer).
void __fastcall LazyTouch(unsigned long long slot, void* ret, BYTE* self)
{
    if (t_lzInBegin || self != g_lzR) return;   // (beginRender's own calls: the deferred textures, +f8)
    LazyRun((int)slot, ret, nullptr);
}

// The vtable's beginRender (slot +10): called by Game::frame (+71f84) before Game::update.
void __fastcall LazyBegin(BYTE* r)
{
    if (g_lz->pending) LazyRun(kSlotBegin, _ReturnAddress(), "at the next frame's start");
    const int kind = g_lzKind ? g_lzKind() : 0;
    const int mode = LazyMode();
    const bool held = mode >= 1 && (kind == 1 || (kind == 2 && mode == 2)) && *(void***)r == g_lzVt;
    const bool busy = (kind == 1 || held) && LazyGpuBusy(r);   // (beginRender here would wait)
    if (kind == 1) ++g_lzSt.framesA;
    if (!held) {
        const double t0 = ProfNow();
        ((PFN_Method)g_lz->orig[kSlotBegin])(r);
        if (kind == 1) {
            const double ms = ProfNow() - t0;
            ++g_lzSt.plain;
            if (busy) ++g_lzSt.plainBusy;
            g_lzSt.plainMs += ms;
            g_lzSt.plainMax = std::max(g_lzSt.plainMax, ms);
        }
        return;
    }
    g_lzR = r;
    g_lzOwnerVal = *(const uint64_t*)(r + g_lzOwner);
    g_lzTid = GetCurrentThreadId();
    g_lzSkipAt = ProfNow();
    ++(kind == 1 ? g_lzSt.heldA : g_lzSt.heldB);
    if (busy) ++g_lzSt.busyStart;
    _InterlockedExchange8(&g_lz->pending, 1);   // (after the fields above: another thread's stub may read them next)
}

} // namespace

// ---------------------------------------------------------------------------------------------------
void InstallHooks(const Targets& t)
{
    g_base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    auto rva = [](BYTE* p) { return p ? (unsigned long long)(p - g_base) : 0ull; };
    if (g_perfTrace >= 1) {
        const bool job = t.jobWait && HookFunction(t.jobWait, (void*)Hook_JobWait, (void**)&Real_JobWait);
        const bool fence = t.fenceWait && HookFunction(t.fenceWait, (void*)Hook_FenceWait, (void**)&Real_FenceWait);
        const bool prof = t.profAdd && HookFunction(t.profAdd, (void*)Hook_ProfAdd, (void**)&Real_ProfAdd);
        const bool disp = t.displayWait && HookFunction(t.displayWait, (void*)Hook_DisplayWait, (void**)&Real_DisplayWait);
        Log("Perf: trace level %d - Job::wait +%llx %s, GPU fence wait +%llx %s, engine profiler +%llx %s, "
            "swap chain frame-latency wait +%llx %s", g_perfTrace,
            rva(t.jobWait), job ? "hooked" : "NOT hooked", rva(t.fenceWait), fence ? "hooked" : "NOT hooked",
            rva(t.profAdd), prof ? "hooked" : "NOT hooked", rva(t.displayWait), disp ? "hooked" : "NOT hooked");
        InstallDetail(t);   // ([Performance] perfDetail)
        if (HANDLE th = CreateThread(nullptr, 64 * 1024, CpuThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
            SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(th);
        }
    }
    // [Performance] jobSpin: the workers' semaphore wait, and Job::wait without perfTrace (with it: hooked above;
    // the main thread's spin runs in that detour).
    if (SpinWanted()) {
        const bool job = g_perfTrace >= 1 ? Real_JobWait != nullptr
                                          : t.jobWait && HookFunction(t.jobWait, (void*)Hook_JobWait, (void**)&Real_JobWait);
        SpinInstall(t, job);
    }
    // The census: perfTrace 2, or frame B work (frameBMode 1 decides from it, mode 2 tells the
    // scripts' registry writes by it).
    if (g_perfTrace < 2 && !t.frameB) return;
    g_entPath = t.entPath;
    const bool tick = t.modTick && HookFunction(t.modTick, (void*)Hook_ModTick, (void**)&Real_ModTick);
    const bool disp = t.dispatcher && HookFunction(t.dispatcher, (void*)Hook_Dispatch, (void**)&Real_Dispatch);
    std::string core;
    for (int i = 0; i < t.coreCount && i < kMaxCore; ++i) {
        g_coreSlot[i] = SlotOf(t.core[i].name);
        const char* n = t.core[i].name;
        g_coreRegWrite[i] = !strcmp(n, "SetBool") || !strcmp(n, "SetInt") || !strcmp(n, "SetFloat") ||
                            !strcmp(n, "SetString") || !strcmp(n, "ClearKey");
        if (g_coreSlot[i] >= 0 && HookFunction(t.core[i].fn, (void*)kCoreHooks[i], (void**)&g_coreReal[i])) {
            g_coreName[i] = n;
            core += std::string(core.empty() ? "" : ", ") + n;
        }
    }
    g_census = tick && disp;
    g_skipReady = g_census && CoreHooked("SetBool") && CoreHooked("SetInt") && CoreHooked("SetFloat") &&
                  CoreHooked("SetString") && CoreHooked("ClearKey");
    // [Performance] frameBKeepMods: parts of mods' script paths, comma separated (lower case here).
    {
        std::string cur;
        for (const char c : Cfg().frameBKeepMods + ",") {
            if (c == ',' || c == ';') {
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                if (!cur.empty()) g_keepMods.push_back(cur);
                cur.clear();
            } else if (c != ' ' || !cur.empty()) {
                cur += (char)tolower((unsigned char)c);
            }
        }
    }
    Log("Perf: mods' ticks +%llx %s, Lua binding dispatcher +%llx %s, core functions: %s%s", rva(t.modTick),
        tick ? "hooked" : "NOT hooked", rva(t.dispatcher), disp ? "hooked" : "NOT hooked", core.empty() ? "none" : core.c_str(),
        g_perfTrace < 2 ? " (for the frame-B work only: nothing timed)" : "");
}

bool Active() { return g_census; }

bool SkipReady() { return g_skipReady; }

bool CoreHooked(const char* name)
{
    for (int i = 0; i < kMaxCore; ++i)
        if (g_coreName[i] && g_coreReal[i] && !strcmp(g_coreName[i], name)) return true;
    return false;
}

void NoteOurScript()
{
    if (g_tickMod && OnMain() && !g_tickMod->ours) {
        g_tickMod->ours = true;
        Log("Perf: our Lua mod is '%s' (its frame-B tick is never skipped)", g_tickMod->name);
    }
}

void TakeSkipStats(int& frames, int& skipped, int& ticks, double& ms)
{
    frames = g_skip.frames;
    skipped = g_skip.skipped;
    ticks = g_skip.ticks;
    ms = g_skip.ms;
    g_skip = SkipStats{};
    if (g_perfTrace >= 2) return;   // (LogDetails cleans the mods up)
    for (auto it = g_mods.begin(); it != g_mods.end();) {
        ModRec& m = it->second;
        if (g_frameNo - m.seen > 3000) {   // (not ticked for ~30 s: another level)
            it = g_mods.erase(it);
            continue;
        }
        m.ms[0] = m.ms[1] = 0;
        m.calls[0] = m.calls[1] = 0;
        ++it;
    }
}

void UpdateBegin(bool frozen)
{
    g_mainTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_updEye = frozen ? 1 : 0;
    g_jobEye.store(g_updEye, std::memory_order_relaxed);   // (perfDetail 2: the eye of the jobs started from now on)
    g_tickMod = nullptr;
    ++g_frameNo;
    if (frozen && FrameB::PairMode() == 1) ++g_skip.frames;
    if (!frozen) g_topMod[0] = g_topMod[1] = TopMod{};
}

std::string SlowFrameMods()
{
    std::string s;
    if (g_topMod[0].ms < 0.5) return s;
    AppendF(s, " ; costliest mod scripts: '%s' %.1f ms", g_topMod[0].name, g_topMod[0].ms);
    if (g_topMod[1].ms >= 0.3) AppendF(s, ", '%s' %.1f ms", g_topMod[1].name, g_topMod[1].ms);
    return s;
}

void NoteSteps(int steps)
{
    if (steps >= 0 && steps < 100) g_stepsFrame = steps;
}

void NoteObjectMotion(int shapes, int moved)
{
    g_shapesLast = shapes;
    g_movedSum += moved;
    ++g_movedPairs;
}

void FrameEnd(int engineEye, double frameMs)
{
    EyeSums& s = g_eye[engineEye == 0 ? 0 : engineEye == 1 ? 1 : 2];
    ++s.frames;
    s.frameMs += frameMs;
    for (int k = 0; k < P_Count; ++k) s.sec[k] += g_prof[k];
    for (int c = 0; c < kCats; ++c) {
        s.cat[c] += g_catFrame[c];
        g_catFrame[c] = 0;
    }
    if (g_stepsFrame >= 0) {
        s.steps += g_stepsFrame;
        ++s.stepFrames;
        g_stepsFrame = -1;
    }
    for (int i = 0; i < g_waitFrameN; ++i) {
        DWORD rva = g_waitFrame[i].rva;
        auto find = [&](DWORD r) { return std::find_if(s.waits.begin(), s.waits.end(), [&](const WaitBy& w) { return w.rva == r; }); };
        auto it = find(rva);
        if (it == s.waits.end() && rva && s.waits.size() >= kWaitEye) it = find(rva = 0);   // (full: "+other")
        if (it == s.waits.end()) {
            s.waits.push_back({ rva, g_waitFrame[i].ms, g_waitFrame[i].n });
        } else {
            it->ms += g_waitFrame[i].ms;
            it->n += g_waitFrame[i].n;
        }
    }
    DetailFrameEnd(engineEye);   // (perfDetail: before this frame's job waits are cleared)
    g_waitFrameN = 0;
}

std::string StatsText()
{
    std::string s;
    if (g_perfTrace < 1) return s;
    const char* eyeName[2] = { "A", "B" };
    for (int e = 0; e < 2; ++e) {
        EyeSums& x = g_eye[e];
        g_lastFrames[e] = x.frames;
        if (!x.frames) continue;
        const double n = x.frames;
        auto avg = [&](ProfSection k) { return x.sec[k] / n; };
        const double known = avg(P_Update) + avg(P_FixedLoop) + avg(P_Particles) + avg(P_Ours) + avg(P_Render) +
                             avg(P_OurPresent) + avg(P_RealPresent) + avg(P_PostPresent);
        AppendF(s, "%s perf %s: frame %.2f = update %.2f, fixed loop %.2f, particles %.2f, ours %.2f, render %.2f, "
                   "VR submit %.2f, present %.2f, headset wait %.2f, rest %.2f; waits: jobs %.2f",
                e ? " |" : ";", eyeName[e], x.frameMs / n, avg(P_Update), avg(P_FixedLoop), avg(P_Particles), avg(P_Ours),
                avg(P_Render), avg(P_OurPresent), avg(P_RealPresent), avg(P_PostPresent), x.frameMs / n - known,
                avg(P_JobWait));
        std::sort(x.waits.begin(), x.waits.end(), [](const WaitBy& a, const WaitBy& b) { return a.ms > b.ms; });
        for (size_t i = 0; i < x.waits.size() && i < 4; ++i)
            AppendWait(s, i ? " " : " (", x.waits[i], x.waits[i].ms / n, "%.2f");
        if (!x.waits.empty()) s += ")";
        AppendF(s, ", GPU %.2f, display %.2f, shapes %.2f", avg(P_GpuWait), avg(P_DisplayWait), avg(P_ShapeWait));
        if (x.stepFrames) AppendF(s, "; physics steps/frame %.2f", x.steps / x.stepFrames);
        bool any = false;
        for (int c = 1; c < kCats; ++c)
            if (x.cat[c] / n >= 0.005) {
                AppendF(s, "%s%s %.2f", any ? ", " : "; engine ms: ", kCatNames[c], x.cat[c] / n);
                any = true;
            }
        x = EyeSums{};
    }
    g_eye[2] = EyeSums{};
    if (g_movedPairs) AppendF(s, " ; scene %d shapes, script moves put back %.1f/pair", g_shapesLast, g_movedSum / g_movedPairs);
    g_movedSum = 0;
    g_movedPairs = 0;
    CpuAcc cpu;
    {
        std::lock_guard<std::mutex> lock(g_cpuMutex);
        cpu = g_cpu;
        g_cpu = CpuAcc{};
    }
    if (cpu.wall > 0.5) {
        const double k = 100.0 / cpu.wall;
        AppendF(s, " ; CPU %% of one core: main %.0f, jobs %.0f (%d threads), engine other %.0f, ours %.0f, other %.0f, "
                   "process %.0f; system busy %.0f%%",
                cpu.cpu[G_Main] * k, cpu.cpu[G_Jobs] * k, cpu.jobThreads, cpu.cpu[G_Engine] * k, cpu.cpu[G_Ours] * k,
                cpu.cpu[G_Other] * k, cpu.process * k, cpu.total > 0 ? cpu.busy / cpu.total * 100.0 : 0.0);
    }
    return s;
}

void LogDetails()
{
    LogInstrument();   // ([Performance] perfDetail: perfTrace 1's details lines)
    if (g_perfTrace < 2) return;
    const char* eyeName[2] = { "A", "B" };
    // Each mod's tick, ms per frame of that eye: the 5 most expensive.
    {
        std::string s = "Perf mods' tick (ms per frame)";
        for (int e = 0; e < 2; ++e) {
            const double n = std::max(1, g_lastFrames[e]);
            std::vector<const ModRec*> v;
            double total = 0;
            for (auto& kv : g_mods)
                if (kv.second.calls[e]) {
                    v.push_back(&kv.second);
                    total += kv.second.ms[e];
                }
            std::sort(v.begin(), v.end(), [&](const ModRec* a, const ModRec* b) { return a->ms[e] > b->ms[e]; });
            AppendF(s, "%s %s: %d mods %.2f", e ? " |" : "", eyeName[e], (int)v.size(), total / n);
            for (size_t i = 0; i < v.size() && i < 5; ++i)
                AppendF(s, "%s'%s' %.2f", i ? ", " : " (", v[i]->name, v[i]->ms[e] / n);
            if (!v.empty()) s += ")";
        }
        Log("%s", s.c_str());
    }
    // Bindings: calls and time per frame, the 6 most expensive.
    {
        std::string s = "Perf Lua bindings (per frame)";
        for (int e = 0; e < 2; ++e) {
            const double n = std::max(1, g_lastFrames[e]);
            double ms = 0, calls = 0;
            std::vector<int> v;
            for (int k = 0; k < kSlots; ++k)
                if (g_bind[e][k].calls) {
                    v.push_back(k);
                    ms += g_bind[e][k].ms;
                    calls += g_bind[e][k].calls;
                }
            std::sort(v.begin(), v.end(), [&](int a, int b) { return g_bind[e][a].ms > g_bind[e][b].ms; });
            AppendF(s, "%s %s: %.0f calls %.2f ms", e ? " |" : "", eyeName[e], calls / n, ms / n);
            for (size_t i = 0; i < v.size() && i < 6; ++i) {
                char nm[64];
                SafeName(g_slotName[v[i]], nm, sizeof(nm));
                AppendF(s, "%s%s %.3f (%.1f)", i ? ", " : " (", nm, g_bind[e][v[i]].ms / n, g_bind[e][v[i]].calls / n);
            }
            if (!v.empty()) s += ")";
            for (int k = 0; k < kSlots; ++k) g_bind[e][k] = BindSums{};
        }
        Log("%s", s.c_str());
    }
    // Census: the bindings each mod called in its ticks, logged when the set grows. "Reads" are the
    // Get / Query / Is / Has / Find bindings; every other one is listed.
    int lines = 0;
    for (auto it = g_mods.begin(); it != g_mods.end();) {
        ModRec& m = it->second;
        if (g_frameNo - m.seen > 3000) {   // (not ticked for ~30 s: another level)
            it = g_mods.erase(it);
            continue;
        }
        if (lines < 30 && ((m.used[0] & ~m.logged[0]).any() || (m.used[1] & ~m.logged[1]).any())) {
            ++lines;
            // Every name is written (no cap: the input of the plan's Step 6 must be complete); a
            // line too long for Log goes on in "Perf census '<mod>' (cont.):" lines.
            std::string s = "Perf census '";
            s += m.name;
            s += "':";
            for (int e = 0; e < 2; ++e) {
                int reads = 0, total = 0;
                std::string others;
                for (int k = 0; k < kSlots; ++k) {
                    if (!m.used[e][k]) continue;
                    ++total;
                    char nm[64];
                    SafeName(g_slotName[k], nm, sizeof(nm));
                    if (IsRead(nm)) {
                        ++reads;
                    } else {
                        others += others.empty() ? "" : " ";
                        others += nm;
                    }
                }
                s += e ? " ; tick " : " tick ";
                s += eyeName[e];
                AppendF(s, " %d bindings, %d reads, ", total, reads);
                s += others.empty() ? "read-only" : "others: ";
                s += others;
                m.logged[e] = m.used[e];
            }
            LogSplit(s, std::string("Perf census '") + m.name + "' (cont.): ");
        }
        for (int e = 0; e < 2; ++e) {
            m.ms[e] = 0;
            m.calls[e] = 0;
        }
        ++it;
    }
}

std::string SlowFrameText(double other)
{
    std::string s;
    if (g_perfTrace < 1) return s;
    const double rest = other - g_prof[P_FixedLoop] - g_prof[P_Particles] - g_prof[P_Ours];
    AppendF(s, " (fixed loop %.1f, particles %.1f, ours %.1f, rest %.1f; waits: jobs %.1f", g_prof[P_FixedLoop],
            g_prof[P_Particles], g_prof[P_Ours], rest, g_prof[P_JobWait]);
    // (the 3 longest waits of this frame, not the first 3 callers: ParticleSystem::finish comes late)
    WaitBy w[kWaitFrame];
    std::copy(g_waitFrame, g_waitFrame + g_waitFrameN, w);
    const int top = std::min(g_waitFrameN, 3);
    std::partial_sort(w, w + top, w + g_waitFrameN, [](const WaitBy& a, const WaitBy& b) { return a.ms > b.ms; });
    for (int i = 0; i < top; ++i) AppendWait(s, " ", w[i], w[i].ms, "%.1f");
    AppendF(s, ", GPU %.1f, display %.1f, shapes %.1f", g_prof[P_GpuWait], g_prof[P_DisplayWait], g_prof[P_ShapeWait]);
    if (g_stepsFrame >= 0) AppendF(s, "; steps %d", g_stepsFrame);
    bool any = false;
    for (int c = 1; c < kCats; ++c)
        if (g_catFrame[c] >= 0.05) {
            AppendF(s, "%s%s %.1f", any ? ", " : "; engine: ", kCatNames[c], g_catFrame[c]);
            any = true;
        }
    s += ")";
    return s;
}

void SessionPriority(bool running)
{
    if (running == g_sessionLast) return;
    g_sessionLast = running;
    HANDLE self = GetCurrentProcess();
    if (running) {
        if (Cfg().cpuPriority <= 0) return;
        // ABOVE_NORMAL only, from NORMAL only: Virtual Desktop's streamer (High) and its own
        // time-critical thread in the game stay above it; never HIGH / REALTIME / MMCSS (they would
        // pre-empt them). A class the player set himself (Task Manager...) is left alone.
        const DWORD cur = GetPriorityClass(self);
        if (cur != NORMAL_PRIORITY_CLASS) {
            Log("Perf: process priority class 0x%lx left as is during the VR session", cur);
        } else if (SetPriorityClass(self, ABOVE_NORMAL_PRIORITY_CLASS)) {
            g_raised = true;
            Log("Perf: process priority \"above normal\" while the VR session runs (cpuPriority=1)");
        } else {
            Log("Perf: process priority not changed (error %lu)", GetLastError());
        }
    } else if (g_raised) {
        g_raised = false;
        // (only if it is still ours: someone may have changed it meanwhile)
        if (GetPriorityClass(self) == ABOVE_NORMAL_PRIORITY_CLASS && SetPriorityClass(self, NORMAL_PRIORITY_CLASS))
            Log("Perf: process priority back to normal (VR session stopped)");
    }
}

// After each Present the engine waits on the swap chain's frame-latency object (TRendererD3D12::endRender ->
// 0x5bc180: WaitForSingleObject, INFINITE), set up with SetMaximumFrameLatency(1) when the swap chain is made.
// With 1 that wait lasts until the GPU has finished the frame just presented. In synchronised stereo the
// CPU therefore starts the right-eye frame B only once the GPU has drawn the left eye, and the GPU then
// waits for frame B's commands: CPU and GPU take turns, each idle while the other works (perfTrace: the
// "display" wait of frame B). With 2, frame B's CPU work runs while the GPU still draws frame A. Nothing
// drawn changes - the same commands in the same order on the same queue -, the engine's own two frames in
// flight (beginFrame's fence) still protect its per-frame resources, and the headset still gets each pair
// at xrEndFrame after frame B. The game's value is put back when the session stops.
void FrameLatency(IDXGISwapChain3* sc, bool session)
{
    static IDXGISwapChain3* s_sc = nullptr;
    static bool s_waitable = false;
    static UINT s_game = 0;     // the game's own value (what is set when it is not ours)
    static UINT s_ours = 0;     // the value we set (0 = none)
    static UINT s_want = 0;
    static unsigned s_n = 0;
    if (!sc) return;
    if (sc != s_sc) {
        s_sc = sc;
        s_game = s_ours = s_want = 0;
        s_n = 0;
        DXGI_SWAP_CHAIN_DESC1 d{};
        s_waitable = SUCCEEDED(sc->GetDesc1(&d)) && (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    }
    if (!s_waitable) return;   // (the engine waits on nothing: SetMaximumFrameLatency is not for this swap chain)
    const int ctl = FrameB::FileLatency();   // (TeardownVR_perf.txt "frameLatency=N", tests)
    const UINT want = session ? (UINT)std::clamp(ctl >= 0 ? ctl : Cfg().frameLatency, 0, 3) : 0;
    // (checked every 30 Presents - the game sets its value again when it remakes the swap chain's
    // buffers -, at once when the wish changes)
    if (s_n++ % 30 != 0 && want == s_want) return;
    s_want = want;
    UINT cur = 0;
    if (FAILED(sc->GetMaximumFrameLatency(&cur)) || !cur) return;
    if (!s_ours || cur != s_ours) s_game = cur;   // not ours: the game's
    const UINT target = want ? want : s_game;
    if (cur == target) {
        s_ours = want ? target : 0;
        return;
    }
    const HRESULT hr = sc->SetMaximumFrameLatency(target);
    if (FAILED(hr)) {
        static int fails = 0;
        if (fails++ < 3) Log("Perf: desktop swap chain frame latency %u -> %u failed (0x%08lx)", cur, target, (unsigned long)hr);
        return;
    }
    s_ours = want ? target : 0;
    Log("Perf: desktop swap chain frame latency %u -> %u (%s)", cur, target,
        want ? "headset session: the right-eye frame is prepared while the GPU draws the left eye" : "the game's value again");
}

// ---------------------------------------------------------------------------------------------------
// Headset stream ([Performance] vdStats, gpuPriority)
// ---------------------------------------------------------------------------------------------------
// Virtual Desktop's runtime (VDXR) hands our frames to Virtual Desktop on its own thread: its
// xrEndFrame first waits until that thread has passed the previous frame on and Virtual Desktop has
// let the next one start (waitForAsyncSubmissionIdle). Every back-pressure of the stream - encoder,
// Wi-Fi, the headset's decoder - therefore holds the game there: a "hold". The "XR: xrEndFrame
// blocked" lines stop after 60 (then one in 50) and cannot be counted; here every xrEndFrame longer
// than one headset refresh is, per "Stereo frames" window and per session, with its duration bucket,
// the bursts holds come in (less than 1 s apart), the codec Virtual Desktop is set to and the rate
// the PC really sent: hold rates can then be compared between codecs, bitrates and settings.
namespace {

struct Holds
{
    int n = 0, bursts = 0;
    double ms = 0, max = 0;
    int bucket[4] = {};   // < 20, 20-40, 40-80, >= 80 ms (about 100: a frame Virtual Desktop never took)
    void Add(double v, bool burst)
    {
        ++n;
        bursts += burst ? 1 : 0;
        ms += v;
        max = std::max(max, v);
        ++bucket[v < 20 ? 0 : v < 40 ? 1 : v < 80 ? 2 : 3];
    }
};
Holds g_holdWin, g_holdSession;   // (main thread)
double g_lastHold = -1e9;         // ProfNow() of the last hold
bool g_streamOn = false;          // a session runs, with vdStats on
bool g_isVd = false;              // ... on Virtual Desktop's runtime
ULONGLONG g_streamStart = 0;
bool g_gpuLowered = false;        // GpuPriority set "below normal" (and puts "normal" back)

// Virtual Desktop's encoder settings, from %ProgramData%\Virtual Desktop\StreamerSettings.json, read
// only. Only CodecName, PreferredCodec and Enable2Pass are taken out of it: the file also holds the
// PC's encrypted account keys, never logged nor kept. The bitrate is a setting of the headset's app
// and is not in this file: the network send rate below shows what was really sent.
struct VdSettings
{
    bool ok = false;
    FILETIME time = {};
    char codec[48] = "";
    int preferred = -1;   // PreferredCodec (Virtual Desktop's own number)
    int twoPass = -1;     // Enable2Pass: 1 / 0 (-1: not in the file)
};
VdSettings g_vd;

// "key": value of a small JSON file - a string (printable ASCII copied, at most n - 1 characters),
// true / false (1 / 0) or an integer. False: that key, or that kind of value, is not there.
bool JsonValue(const char* json, const char* key, char* str, size_t n, int* num)
{
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char* p = strstr(json, quoted);
    if (!p) return false;
    p += strlen(quoted);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p == '"') {
        if (!str || !n) return false;
        size_t i = 0;
        for (++p; *p && *p != '"' && i + 1 < n; ++p) str[i++] = *p >= 32 && *p < 127 ? *p : '?';
        str[i] = 0;
        return true;
    }
    if (!num) return false;
    if (!strncmp(p, "true", 4)) *num = 1;
    else if (!strncmp(p, "false", 5)) *num = 0;
    else if ((*p >= '0' && *p <= '9') || *p == '-') *num = atoi(p);
    else return false;
    return true;
}

// Reads the file again when its time changed (main thread: one file-time check, ~20 us; Virtual
// Desktop also rewrites it for other keys). True when the three settings differ from the last read.
bool ReadVdSettings()
{
    wchar_t path[MAX_PATH];
    const DWORD len = ExpandEnvironmentStringsW(L"%ProgramData%\\Virtual Desktop\\StreamerSettings.json", path, MAX_PATH);
    if (!len || len > MAX_PATH) return false;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return false;
    if (g_vd.ok && CompareFileTime(&fa.ftLastWriteTime, &g_vd.time) == 0) return false;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;   // (being written: read at the next line)
    std::vector<char> buf(64 * 1024);
    DWORD got = 0;
    const BOOL read = ReadFile(h, buf.data(), (DWORD)buf.size() - 1, &got, nullptr);
    CloseHandle(h);
    if (!read) return false;
    buf[got] = 0;
    VdSettings s;
    s.ok = true;
    s.time = fa.ftLastWriteTime;
    JsonValue(buf.data(), "CodecName", s.codec, sizeof(s.codec), nullptr);
    JsonValue(buf.data(), "PreferredCodec", nullptr, 0, &s.preferred);
    JsonValue(buf.data(), "Enable2Pass", nullptr, 0, &s.twoPass);
    SecureZeroMemory(buf.data(), buf.size());   // (the account keys are not left in our memory)
    const bool changed = !g_vd.ok || strcmp(s.codec, g_vd.codec) != 0 || s.preferred != g_vd.preferred ||
                         s.twoPass != g_vd.twoPass;
    g_vd = s;
    return changed;
}

void LogVdSettings(const char* what)
{
    Log("VD: encoder settings%s (StreamerSettings.json, read only): codec \"%s\" (PreferredCodec %d), 2-pass %s. The "
        "bitrate is set in the headset: the \"sent\" Mbit/s of the Stereo frames lines is what the PC really sent",
        what, g_vd.codec[0] ? g_vd.codec : "?", g_vd.preferred,
        g_vd.twoPass == 1 ? "on" : g_vd.twoPass == 0 ? "off" : "?");
}

// The PC's network send rate, all wired and Wi-Fi adapters together (virtual ones left out: a Hyper-V
// switch would count the same bytes twice), sampled once per second on a background thread
// (GetIfTable2 takes 0.1-1 ms: never on the game's threads). With the PC wired to the router it is
// almost all Virtual Desktop's video: its real bitrate (the headset's setting is a ceiling, which the
// automatic bitrate moves), and with a quality-driven rate control the effect of hiddenAreaFill.
std::atomic<unsigned> g_netGen{ 0 };     // a sampler runs while this holds its number
std::atomic<uint64_t> g_netBits{ 0 };    // bits sent since the last "Stereo frames" line...
std::atomic<uint64_t> g_netMs{ 0 };      // ... over this many milliseconds
std::atomic<unsigned> g_netMax{ 0 };     // the highest one-second rate meanwhile, Mbit/s
uint64_t g_netSessBits = 0, g_netSessMs = 0;   // the session's (main thread, from the windows)

DWORD WINAPI NetThread(void* arg)
{
    const unsigned gen = (unsigned)(uintptr_t)arg;
    using PFN_GetIfTable2 = DWORD(WINAPI*)(PMIB_IF_TABLE2*);
    using PFN_FreeMibTable = VOID(WINAPI*)(PVOID);
    const HMODULE m = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);   // (never the game folder's)
    const auto getTable = m ? (PFN_GetIfTable2)GetProcAddress(m, "GetIfTable2") : nullptr;
    const auto freeTable = m ? (PFN_FreeMibTable)GetProcAddress(m, "FreeMibTable") : nullptr;
    if (!getTable || !freeTable) {
        Log("VD: network send rate not available (iphlpapi.dll)");
        return 0;
    }
    uint64_t last = 0;
    ULONGLONG lastT = 0;
    while (g_netGen.load() == gen) {
        PMIB_IF_TABLE2 t = nullptr;
        if (getTable(&t) == NO_ERROR && t) {
            uint64_t sum = 0;
            for (ULONG i = 0; i < t->NumEntries; ++i) {
                const MIB_IF_ROW2& r = t->Table[i];
                // (an adapter's filter modules - QoS, WFP - are listed with the adapter's own counters)
                if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface ||
                    r.OperStatus != IfOperStatusUp)
                    continue;
                if (r.Type != IF_TYPE_ETHERNET_CSMACD && r.Type != IF_TYPE_IEEE80211) continue;
                sum += r.OutOctets;
            }
            freeTable(t);
            const ULONGLONG now = GetTickCount64();
            // (an adapter that went down makes the sum smaller: that second is left out)
            if (lastT && now > lastT && sum >= last) {
                const uint64_t bits = (sum - last) * 8;
                g_netBits += bits;
                g_netMs += now - lastT;
                const unsigned mbps = (unsigned)((double)bits / 1000.0 / (double)(now - lastT));
                unsigned cur = g_netMax.load();
                while (mbps > cur && !g_netMax.compare_exchange_weak(cur, mbps)) {}
            }
            last = sum;
            lastT = now;
        }
        Sleep(1000);
    }
    return 0;
}

// D3DKMT process GPU scheduling classes (d3dkmthk.h): 0 idle, 1 below normal, 2 normal, 3 above normal,
// 4 high, 5 realtime. gdi32 exports both functions (kernel thunks); they return an NTSTATUS.
using PFN_GetGpuClass = LONG(APIENTRY*)(HANDLE, int*);
using PFN_SetGpuClass = LONG(APIENTRY*)(HANDLE, int);
PFN_GetGpuClass g_getGpuClass = nullptr;
PFN_SetGpuClass g_setGpuClass = nullptr;

// Virtual Desktop's streamer's GPU class, read once (a background thread: the process list takes a few
// ms). If it already runs above "normal", lowering the game's class changes nothing.
DWORD WINAPI StreamerGpuClassThread(void*)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (!_wcsicmp(pe.szExeFile, L"VirtualDesktop.Streamer.exe")) {
                pid = pe.th32ProcessID;
                break;
            }
        CloseHandle(snap);
    }
    if (!pid) {
        Log("Perf: Virtual Desktop's streamer not found (its GPU class not read)");
        return 0;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    int cls = -1;
    const LONG st = h && g_getGpuClass ? g_getGpuClass(h, &cls) : -1;
    if (h) CloseHandle(h);
    Log("Perf: Virtual Desktop's streamer GPU class %d (NTSTATUS 0x%08lx; 2 normal, 3 above normal, 4 high, "
        "5 realtime, -1 not readable without its rights)", st >= 0 ? cls : -1, (unsigned long)st);
    return 0;
}

} // namespace

void StreamSession(bool running, const std::string& runtime)
{
    static bool s_last = false;
    if (running == s_last) return;
    s_last = running;
    if (running) {
        g_streamOn = Cfg().vdStats;
        if (!g_streamOn) return;
        g_holdWin = g_holdSession = Holds{};
        g_lastHold = -1e9;
        g_streamStart = GetTickCount64();
        g_netSessBits = g_netSessMs = 0;
        g_netBits = 0;
        g_netMs = 0;
        g_netMax = 0;
        g_isVd = runtime.find("VirtualDesktop") != std::string::npos;
        if (g_isVd) {
            g_vd = VdSettings{};
            if (ReadVdSettings()) LogVdSettings("");
            else Log("VD: StreamerSettings.json not read (codec unknown on the Stereo frames lines)");
        }
        const unsigned gen = ++g_netGen;
        if (HANDLE th = CreateThread(nullptr, 64 * 1024, NetThread, (void*)(uintptr_t)gen,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
            SetThreadPriority(th, THREAD_PRIORITY_LOWEST);
            CloseHandle(th);
        }
        Log("VD: stream statistics on (vdStats=1): every xrEndFrame longer than one headset refresh is a hold, counted "
            "with its duration on each Stereo frames line; the PC's network send rate is sampled once per second");
        return;
    }
    if (!g_streamOn) return;
    g_streamOn = false;
    ++g_netGen;   // (the sampler ends within a second)
    g_netSessBits += g_netBits.exchange(0);
    g_netSessMs += g_netMs.exchange(0);
    const Holds& h = g_holdSession;
    const double s = (double)(GetTickCount64() - g_streamStart) / 1000.0;
    Log("VD: session totals over %.0f s: %d holds in %d bursts (%.2f holds per minute; %.0f ms in all, max %.0f; "
        "<20 ms %d, 20-40 %d, 40-80 %d, 80+ %d), sent %.0f Mbit/s on average%s%s%s",
        s, h.n, h.bursts, s > 0 ? h.n * 60.0 / s : 0.0, h.ms, h.max, h.bucket[0], h.bucket[1], h.bucket[2], h.bucket[3],
        g_netSessMs ? (double)g_netSessBits / 1000.0 / (double)g_netSessMs : 0.0, g_isVd && g_vd.ok ? ", last codec " : "",
        g_isVd && g_vd.ok ? g_vd.codec : "", g_isVd && g_vd.ok && g_vd.twoPass == 1 ? " 2-pass" : "");
}

void NoteEndFrame(double ms, double periodMs)
{
    if (!g_streamOn) return;
    // (longer than one refresh: the criterion of "while the runtime held the frame" in vr.cpp)
    if (ms <= (periodMs > 1.0 ? periodMs : 1000.0 / 90.0)) return;
    const double now = ProfNow();
    const bool burst = now - g_lastHold > 1000.0;
    g_lastHold = now;
    g_holdWin.Add(ms, burst);
    g_holdSession.Add(ms, burst);
}

std::string StreamStatsText(double fillPct)
{
    if (!g_streamOn) return "";
    std::string s;
    const Holds& h = g_holdWin;
    AppendF(s, " ; %s holds %d in %d bursts (%.0f ms, max %.0f; <20:%d 20-40:%d 40-80:%d 80+:%d)",
            g_isVd ? "VD" : "runtime", h.n, h.bursts, h.ms, h.max, h.bucket[0], h.bucket[1], h.bucket[2], h.bucket[3]);
    const uint64_t bits = g_netBits.exchange(0), ms = g_netMs.exchange(0);
    const unsigned top = g_netMax.exchange(0);
    g_netSessBits += bits;
    g_netSessMs += ms;
    if (ms) AppendF(s, ", sent %.0f Mbit/s (1 s max %u)", (double)bits / 1000.0 / (double)ms, top);
    if (g_isVd) {
        // (the player may change the codec on the PC between two levels: a line, and the new tag)
        if (ReadVdSettings()) LogVdSettings(" changed");
        if (g_vd.ok) AppendF(s, ", %s%s", g_vd.codec[0] ? g_vd.codec : "codec ?", g_vd.twoPass == 1 ? " 2-pass" : "");
    }
    if (fillPct > 0) AppendF(s, ", hidden area %.1f%% black", fillPct);
    if (g_gpuLowered) s += ", GPU class below normal";
    g_holdWin = Holds{};
    return s;
}

// Lowering the game's GPU class lets Virtual Desktop's streamer - another process, which converts and
// encodes each frame on the same GPU - pre-empt the game's queued frames when the GPU is saturated
// (with frameLatency 2 the game keeps up to two frames of work queued). Nothing drawn changes: the
// same commands in the same order on the same queue; only which process the GPU serves first. Only
// the GPU-saturated kind of hold can shorten (heavy destruction); the holds that come from the Wi-Fi
// or the headset's decoder - most of them, the encoder idles during them - cannot. Other programs of
// normal class (a browser's video, an AI job) would pre-empt the game too: off by default.
void GpuPriority(bool running)
{
    static int s_want = 0;
    static bool s_resolved = false;
    const int ctl = FrameB::FileGpuPriority();   // (TeardownVR_perf.txt "gpuPriority=N": A/B tests)
    const int want = running ? std::clamp(ctl >= 0 ? ctl : Cfg().gpuPriority, 0, 1) : 0;
    if (want == s_want) return;
    s_want = want;
    if (!s_resolved) {
        s_resolved = true;
        if (HMODULE g = GetModuleHandleW(L"gdi32.dll")) {   // (loaded in every D3D game)
            g_getGpuClass = (PFN_GetGpuClass)GetProcAddress(g, "D3DKMTGetProcessSchedulingPriorityClass");
            g_setGpuClass = (PFN_SetGpuClass)GetProcAddress(g, "D3DKMTSetProcessSchedulingPriorityClass");
        }
    }
    if (!g_getGpuClass || !g_setGpuClass) {
        if (want) Log("Perf: GPU scheduling class not available (gdi32 D3DKMT functions not found)");
        return;
    }
    HANDLE self = GetCurrentProcess();
    int cur = -1;
    const LONG gs = g_getGpuClass(self, &cur);
    if (want) {
        // Below normal only, from normal only: a class the player set himself (Process Lasso...) is left alone.
        if (gs < 0 || cur != 2) {
            Log("Perf: GPU scheduling class %d left as is during the VR session (NTSTATUS 0x%08lx)", cur, (unsigned long)gs);
            return;
        }
        const LONG st = g_setGpuClass(self, 1);
        if (st < 0) {
            Log("Perf: GPU scheduling class not lowered (NTSTATUS 0x%08lx)", (unsigned long)st);
            return;
        }
        g_gpuLowered = true;
        Log("Perf: GPU scheduling class \"below normal\" while the VR session runs (gpuPriority=1%s): Virtual Desktop's "
            "streamer, another process, may pre-empt the game's GPU work", ctl >= 0 ? ", TeardownVR_perf.txt" : "");
        if (HANDLE th = CreateThread(nullptr, 64 * 1024, StreamerGpuClassThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                     nullptr)) {
            SetThreadPriority(th, THREAD_PRIORITY_LOWEST);
            CloseHandle(th);
        }
    } else if (g_gpuLowered) {
        g_gpuLowered = false;
        // (only if it is still ours: someone may have changed it meanwhile)
        if (gs >= 0 && cur == 1) {
            const LONG st = g_setGpuClass(self, 2);
            Log("Perf: GPU scheduling class back to normal (%s; NTSTATUS 0x%08lx)",
                running ? "gpuPriority=0" : "VR session stopped", (unsigned long)st);
        }
    }
}

void CheckOverlays()
{
    const bool steam = GetModuleHandleW(L"gameoverlayrenderer64.dll") != nullptr;
    const bool nv = GetModuleHandleW(L"nvspcap64.dll") != nullptr;
    const char* names = steam && nv ? "Steam + NVIDIA" : steam ? "Steam" : nv ? "NVIDIA" : nullptr;
    const char* was = g_overlay.exchange(names);
    if (names && names != was)
        Log("Warning: overlay hooks loaded in the game (%s%s%s). They wrap every Present: the Steam overlay was seen "
            "blocking 4-7 ms in it. For VR, turn off the Steam overlay for Teardown and NVIDIA's overlay / Instant "
            "Replay (see LISEZMOI)", steam ? "gameoverlayrenderer64.dll" : "", steam && nv ? ", " : "",
            nv ? "nvspcap64.dll" : "");
}

const char* OverlayNames() { return g_overlay.load(); }

// ---------------------------------------------------------------------------------------------------
void LazyBeginInstall(BYTE* beginRender, BYTE* endRender, int (*frameKind)())
{
    if (g_lz) return;
    if (!g_base) {
        g_base = (BYTE*)GetModuleHandleW(nullptr);
        auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
        g_imageSize = nt->OptionalHeader.SizeOfImage;
    }
    auto fail = [](const char* why) {
        Log("Perf: lazy beginFrame NOT available (%s) - every frame A waits for the GPU at its start, as before", why);
    };
    if (!beginRender || !endRender) return fail("beginRender / endRender not recognised");
    // (the offsets beginRender's pattern holds, engine.cpp: the frame's thread +f70, context index +da8,
    // contexts +ef8, fence +e58)
    memcpy(&g_lzOwner, beginRender + 0x24, 4);
    memcpy(&g_lzCtxIdx, beginRender + 0x2b, 4);
    memcpy(&g_lzCtxs, beginRender + 0x3a, 4);
    memcpy(&g_lzFence, beginRender + 0x41, 4);
    // Its vtable: the one qword of .rdata equal to beginRender (slot +10), endRender at +18, 74 methods in
    // .text and no 75th, the RTTI name TRendererD3D12.
    auto nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    BYTE *text = nullptr, *textEnd = nullptr;
    void** vt = nullptr;
    int hits = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        BYTE* lo = g_base + sec->VirtualAddress;
        if (strncmp((const char*)sec->Name, ".text", 5) == 0) {
            text = lo;
            textEnd = lo + sec->Misc.VirtualSize;
        }
        if (strncmp((const char*)sec->Name, ".rdata", 6) != 0) continue;
        for (BYTE* p = lo + 2 * sizeof(void*); p + sizeof(void*) <= lo + sec->Misc.VirtualSize; p += sizeof(void*))
            if (*(BYTE**)p == beginRender) {
                vt = (void**)p - kSlotBegin;
                ++hits;
            }
    }
    auto inText = [&](const void* f) { return (const BYTE*)f >= text && (const BYTE*)f < textEnd; };
    if (hits != 1 || !text || vt[kSlotBegin + 1] != endRender) return fail("its vtable not found");
    for (int i = 0; i < kRendSlots; ++i)
        if (!inText(vt[i])) return fail("its vtable is shorter than expected");
    if (inText(vt[kRendSlots])) return fail("its vtable is longer than expected");
    const BYTE* col = (const BYTE*)vt[-1];   // RTTI complete object locator: signature 1, type descriptor RVA at +0C
    const BYTE* imageEnd = g_base + g_imageSize;
    const char* name = nullptr;
    if (col >= g_base && col + 0x18 <= imageEnd && *(const DWORD*)col == 1) {
        const BYTE* td = g_base + *(const DWORD*)(col + 0x0c);
        if (td >= g_base && td + 0x30 <= imageEnd) name = (const char*)td + 0x10;
    }
    if (!name || strncmp(name, ".?AVTRendererD3D12@@", 21) != 0) return fail("not TRendererD3D12's vtable");
    // Methods left as they are (they touch no frame context): the empty ones (ret 0); the creations whose
    // data goes through the async uploader - +100 and +110 (buffers), +120 (texture), exactly these
    // functions (a hash of their bytes, Teardown 2.0.1; scripts' Shoot / Spawn and the player's flashlight
    // create lights with them during the update); two getters (+160 timestamp frequency, +178 a constant).
    // Every other method gets a stub; beginRender itself becomes LazyBegin.
    auto fnv = [](const BYTE* p, size_t n) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
        return h;
    };
    auto bodyHash = [&](const void* f) -> uint64_t {
        DWORD64 ib = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)f, &ib, nullptr);
        if (!rf || (BYTE*)ib != g_base || g_base + rf->BeginAddress != (const BYTE*)f) return 0;
        return fnv((const BYTE*)f, rf->EndAddress - rf->BeginAddress);
    };
    bool keep[kRendSlots] = {};
    for (int i = 0; i < kRendSlots; ++i) {
        const BYTE* f = (const BYTE*)vt[i];
        keep[i] = f[0] == 0xC3 || (f[0] == 0xC2 && f[1] == 0 && f[2] == 0);
    }
    keep[0x100 / 8] = bodyHash(vt[0x100 / 8]) == 0x28A97FE863E1973Cull;
    keep[0x110 / 8] = bodyHash(vt[0x110 / 8]) == 0x883E77854AEB3566ull;
    keep[0x120 / 8] = bodyHash(vt[0x120 / 8]) == 0x8328E8A1C845BEC1ull;
    keep[0x160 / 8] = memcmp(vt[0x160 / 8], "\x48\x8B\x81\x68\x0F\x00\x00\xC3", 8) == 0;
    keep[0x178 / 8] = memcmp(vt[0x178 / 8], "\x8B\x81\xB0\x0F\x00\x00\xC3", 7) == 0;
    keep[kSlotBegin] = false;
    // The stubs (one page of code, then the data page). Per method, 32 bytes:
    //   cmp byte [pending],0 ; jne slow ; jmp [orig+i*8]      (nothing held back: straight to the method)
    //   slow: push i ; jmp common
    // common saves the argument registers (rcx rdx r8 r9, xmm0-5), calls LazyTouch(i, caller, this),
    // restores them and jumps to orig[i] with the caller's stack untouched.
    BYTE* mem = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return fail("no memory for its stubs");
    BYTE* code = mem;
    LazyPage* pg = (LazyPage*)(mem + 0x1000);
    BYTE* common = code + kRendSlots * kStubSize;
    auto rel = [](const BYTE* next, const void* to) { return (int32_t)((const BYTE*)to - next); };
    auto put = [](BYTE*& p, std::initializer_list<int> b) {
        for (int v : b) *p++ = (BYTE)v;
    };
    auto put32 = [](BYTE*& p, int32_t v) {
        memcpy(p, &v, 4);
        p += 4;
    };
    memset(code, 0xCC, 0x1000);
    for (int i = 0; i < kRendSlots; ++i) {
        pg->orig[i] = vt[i];
        BYTE* s = code + i * kStubSize;
        BYTE* p = s;
        put(p, { 0x80, 0x3D });                                        // cmp byte [rip+pending], 0
        put32(p, rel(s + 7, (const void*)&pg->pending));
        put(p, { 0x00 });
        put(p, { 0x0F, 0x85 });                                        // jne slow
        put32(p, rel(s + 13, s + 19));
        put(p, { 0xFF, 0x25 });                                        // jmp [rip+orig+i*8]
        put32(p, rel(s + 19, &pg->orig[i]));
        put(p, { 0x68 });                                              // slow: push i
        put32(p, i);
        put(p, { 0xE9 });                                              // jmp common
        put32(p, rel(s + 29, common));
    }
    BYTE* c = common;
    put(c, { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51 });                    // push rcx ; push rdx ; push r8 ; push r9
    put(c, { 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 });              // sub rsp, 80h (rsp 16-aligned again)
    for (int k = 0; k < 6; ++k)                                        // movdqu [rsp+20h+10h*k], xmmk
        put(c, { 0xF3, 0x0F, 0x7F, 0x44 | (k << 3), 0x24, 0x20 + 0x10 * k });
    put(c, { 0x48, 0x8B, 0x8C, 0x24, 0xA0, 0x00, 0x00, 0x00 });        // mov rcx, [rsp+0A0h]   (i)
    put(c, { 0x48, 0x8B, 0x94, 0x24, 0xA8, 0x00, 0x00, 0x00 });        // mov rdx, [rsp+0A8h]   (the caller's return address)
    put(c, { 0x4C, 0x8B, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00 });        // mov r8, [rsp+98h]     (this)
    put(c, { 0x48, 0xB8 });                                            // mov rax, LazyTouch
    const uint64_t touch = (uint64_t)&LazyTouch;
    memcpy(c, &touch, 8);
    c += 8;
    put(c, { 0xFF, 0xD0 });                                            // call rax
    for (int k = 0; k < 6; ++k)                                        // movdqu xmmk, [rsp+20h+10h*k]
        put(c, { 0xF3, 0x0F, 0x6F, 0x44 | (k << 3), 0x24, 0x20 + 0x10 * k });
    put(c, { 0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00 });              // add rsp, 80h
    put(c, { 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 });              // pop r9 ; pop r8 ; pop rdx ; pop rcx ; pop rax (i)
    BYTE* lea = c;
    put(c, { 0x4C, 0x8D, 0x1D });                                      // lea r11, [rip+orig]
    put32(c, rel(lea + 7, pg->orig));
    put(c, { 0x41, 0xFF, 0x24, 0xC3 });                                // jmp [r11+rax*8]
    // (unwind data for common, so that an exception raised under LazyTouch unwinds through it: version 1,
    // no prolog, UWOP_ALLOC_LARGE of 21 x 8 = A8h - the pushes and the 80h, nothing to restore)
    const BYTE uw[8] = { 0x01, 0x00, 0x02, 0x00, 0x00, 0x01, 0x15, 0x00 };
    memcpy(pg->unwind, uw, sizeof(uw));
    pg->fn.BeginAddress = (DWORD)(common - mem);
    pg->fn.EndAddress = (DWORD)(c - mem);
    pg->fn.UnwindData = (DWORD)(pg->unwind - mem);
    DWORD old = 0;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old)) return fail("its stubs could not be made executable");
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    RtlAddFunctionTable(&pg->fn, 1, (DWORD64)mem);
    g_lz = pg;
    g_lzVt = vt;
    g_lzKind = frameKind;
    if (!VirtualProtect(vt, kRendSlots * sizeof(void*), PAGE_READWRITE, &old)) {
        g_lz = nullptr;
        return fail("its vtable is not writable");
    }
    // (main thread, between two frames: no other thread uses the renderer now)
    int guarded = 0;
    for (int i = 0; i < kRendSlots; ++i) {
        if (i == kSlotBegin) {
            vt[i] = (void*)&LazyBegin;
        } else if (!keep[i]) {
            vt[i] = code + i * kStubSize;
            ++guarded;
        }
    }
    VirtualProtect(vt, kRendSlots * sizeof(void*), old, &old);
    auto rva = [](const void* q) { return (unsigned long long)((const BYTE*)q - g_base); };
    Log("Perf: lazy beginFrame ready - TRendererD3D12 vtable +%llx: beginRender +%llx can wait for the first renderer use, "
        "%d methods guarded, %d left as they are (empty ones, getters, async-uploader creations%s); [Performance] "
        "lazyBeginFrame=%d%s", rva(vt), rva(beginRender), guarded, kRendSlots - 1 - guarded,
        keep[0x100 / 8] && keep[0x110 / 8] && keep[0x120 / 8] ? "" : " NOT recognised: guarded too", Cfg().lazyBeginFrame,
        FrameB::FileLazyBegin() >= 0 ? " (the control file TeardownVR_perf.txt overrides it)" : "");
}

std::string LazyStatsText()
{
    std::string s;
    if (!g_lz) return s;
    LazyStats& x = g_lzSt;
    AppendF(s, " ; lazy beginFrame %d%s:", std::max(g_lzMode, 0), g_lzOff ? " (OFF)" : "");
    const int held = x.heldA + x.heldB;
    if (held) {
        AppendF(s, " held %d/%d A", x.heldA, x.framesA);
        if (x.heldB) AppendF(s, " + %d B", x.heldB);
        AppendF(s, " (GPU busy %d), first use +%.2f ms (", x.busyStart, x.gapMs / held);
        std::sort(x.use, x.use + x.uses, [](const LazyStats::Use& a, const LazyStats::Use& b) { return a.n > b.n; });
        for (int i = 0; i < x.uses && i < 2; ++i)
            AppendF(s, "%s+%x by +%x %d", i ? ", " : "", x.use[i].slot * 8, x.use[i].rva, x.use[i].n);
        AppendF(s, "), beginRender %.2f ms (max %.2f, busy %d)", x.beginMs / held, x.beginMax, x.busyUse);
    }
    if (x.plain)
        AppendF(s, " A beginRender %.2f ms (max %.2f, GPU busy %d/%d)", x.plainMs / x.plain, x.plainMax, x.plainBusy,
                x.plain);
    x = LazyStats{};
    return s;
}

// ---------------------------------------------------------------------------------------------------
// Job-wait latency (the section "Job-wait latency" above)
// ---------------------------------------------------------------------------------------------------
bool SpinWanted()
{
    // (once, before the hooks: the ini's jobSpin, or the control file there at the game's start - its "jobSpin=N"
    // then switches it live, as for frameBMode)
    static int wanted = -1;
    if (wanted < 0) {
        const std::wstring ctl = ModDirectory() + L"TeardownVR_perf.txt";
        wanted = Cfg().jobSpin > 0 || GetFileAttributesW(ctl.c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
    }
    return wanted == 1;
}

void SpinFrameA(bool vr)
{
    if (!g_spinWorkers && !g_spinMain) return;
    g_spinMainTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_spinMainCpu.store((int)GetCurrentProcessorNumber(), std::memory_order_relaxed);
    const int mode = std::clamp(g_spinFile >= 0 ? g_spinFile : g_spinIni, 0, 3);
    const int us = std::clamp(g_spinFileUs >= 0 ? g_spinFileUs : Cfg().jobSpinUs, 0, 200);
    const int mainUs = std::clamp(g_spinFileMainUs >= 0 ? g_spinFileMainUs : Cfg().mainSpinUs, 0, 200);
    // (the headset session only: g_sessionLast is SessionPriority's, every Present)
    const bool on = vr && g_sessionLast;
    const uint64_t w = on && (mode & 1) && g_spinWorkers ? SpinTicks(us) : 0;
    const uint64_t m = on && (mode & 2) && g_spinMain ? SpinTicks(mainUs) : 0;
    g_spinWorkerMax.store(w, std::memory_order_relaxed);
    g_spinMainMax.store(m, std::memory_order_relaxed);
    // (closed at this frame's Present; after 100 ms by itself, should that Present not come through vr.cpp)
    g_spinUntil.store(w || m ? __rdtsc() + SpinTicks(100000.0) : 0, std::memory_order_relaxed);
    if (!w && !m) return;
    ++g_spinFramesA;
    static long long s_logged = -1;
    const long long key = ((long long)(w ? us + 1 : 0) << 16) | (m ? mainUs + 1 : 0);
    if (key != s_logged) {
        s_logged = key;
        char a[32] = "off", b[32] = "off";
        if (w) snprintf(a, sizeof(a), "up to %d us", us);
        if (m) snprintf(b, sizeof(b), "up to %d us", mainUs);
        Log("Perf: job spin on in frame A of the headset session - idle job threads: %s, the main thread in Job::wait: %s "
            "(%s)", a, b, g_mwaitx ? "MWAITX naps" : "PAUSE");
    }
}

void SpinPresent()
{
    if (!g_spinWorkers && !g_spinMain) return;
    g_spinUntil.store(0, std::memory_order_relaxed);
    if (++g_spinPresents < 600) return;
    g_spinPresents = 0;
    if (!g_spinFramesA) return;
    WorkerSpin s, d;
    const int n = std::min(g_workerSpinCount.load(std::memory_order_relaxed), kSpinThreads);
    for (int i = 0; i < n; ++i) {
        const WorkerSpin& x = g_workerSpin[i];
        s.idle += x.idle;
        s.woken += x.woken;
        s.slept += x.slept;
        s.skipped += x.skipped;
        s.ticks += x.ticks;
        s.wokenTicks += x.wokenTicks;
    }
    const WorkerSpin& l = g_workerSpinLast;
    d.idle = s.idle - l.idle;
    d.woken = s.woken - l.woken;
    d.slept = s.slept - l.slept;
    d.skipped = s.skipped - l.skipped;
    d.ticks = s.ticks - l.ticks;
    d.wokenTicks = s.wokenTicks - l.wokenTicks;
    g_workerSpinLast = s;
    const double fa = g_spinFramesA;
    auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * (double)a / (double)b : 0.0; };
    auto avgUs = [](uint64_t ticks, uint64_t k) { return k ? (double)ticks / g_tscPerUs / (double)k : 0.0; };
    const MainSpin& m = g_mainSpin;
    // (what proves it works: "woken while spinning" - units taken without a sleep; "done while spinning" - joins
    // without a block)
    Log("Perf: job spin, %d frames A: idle job threads %.1f per frame A - %.0f%% woken while spinning (after %.1f us), "
        "%.0f%% slept after spinning, %.0f%% not spun (beside the main thread, frame A over), their spinning %.2f ms of CPU "
        "per frame A; main thread %.1f Job::wait per frame A - %.0f%% done while spinning (after %.1f us), %.0f%% spun then "
        "blocked, %.0f%% not spun (long waits %.0f%%, nothing to spin on %.0f%%), its spinning %.3f ms per frame A",
        g_spinFramesA, (double)d.idle / fa, pct(d.woken, d.idle), avgUs(d.wokenTicks, d.woken), pct(d.slept, d.idle),
        pct(d.skipped, d.idle), (double)d.ticks / g_tscPerUs / 1000.0 / fa, (double)m.waits / fa, pct(m.caught, m.waits),
        avgUs(m.caughtTicks, m.caught), pct(m.blocked, m.waits), pct(m.longWaits + m.other, m.waits),
        pct(m.longWaits, m.waits), pct(m.other, m.waits), (double)m.ticks / g_tscPerUs / 1000.0 / fa);
    g_spinFramesA = 0;
    g_mainSpin = MainSpin{};
}

void SpinControl(const char* text)
{
    const int mode = text ? std::min(SpinKey(text, "jobSpin"), 3) : -1;
    const int us = text ? SpinKey(text, "jobSpinUs") : -1;
    const int mainUs = text ? SpinKey(text, "mainSpinUs") : -1;
    if (mode == g_spinFile && us == g_spinFileUs && mainUs == g_spinFileMainUs) return;
    g_spinFile = mode;
    g_spinFileUs = us;
    g_spinFileMainUs = mainUs;
    Log("Perf: control file TeardownVR_perf.txt: jobSpin=%d jobSpinUs=%d mainSpinUs=%d (-1: the ini's)%s", mode, us, mainUs,
        g_spinWorkers || g_spinMain ? "" : " - no effect: job spin was not installed at the game's start");
}

bool PassesWanted() { return (DetailWanted() & kDetailPasses) != 0; }

void GpuPassMark(void* backend, int where)
{
    if (!(g_detail.load(std::memory_order_relaxed) & kDetailPasses) || !backend || !OnMain()) return;
    g_passBackend = (BYTE*)backend;
    g_passInRender = where == 1;
    PassMark(g_passBackend, where == 1 ? kPassRender : kPassAfter);
}

void GpuQueue(ID3D12CommandQueue* q)
{
    if (g_gpu.tried || !q || !(g_detailHooks & kDetailGpu)) return;
    g_gpu.tried = true;
    g_gpu.ok = GpuInit(q);
    if (!g_gpu.ok) {
        g_detailHooks &= ~kDetailGpu;
        g_detail.fetch_and(~kDetailGpu, std::memory_order_relaxed);
    }
    if (!g_gpu.passOk && (g_detailHooks & kDetailPasses)) {   // (perfDetail 8 needs its own objects too)
        g_detailHooks &= ~kDetailPasses;
        g_detail.fetch_and(~kDetailPasses, std::memory_order_relaxed);
    } else if (g_gpu.passOk) {
        Log("Perf: GPU time per pass on: %d timestamps per frame at most, read with the frame's submission timestamps",
            kPassMarks);
    }
}

bool GpuSubmit(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, bool ours, PFN_Ecl real)
{
    GpuState& g = g_gpu;
    if (!(g_detail.load(std::memory_order_relaxed) & kDetailGpu) || !g.ok || q != g.queue || !n || n > 60 || !OnMain())
        return false;
    GpuFrame& f = g.f[g.cur];
    // (the frame's slot must have been read - kGpuRing frames ago -, else this frame goes untimed)
    const bool mark = !f.used && f.n + 2 <= kGpuMarks;
    // (f.used: the slot still holds an older frame, not read yet - that frame was timed whole; this one goes
    // untimed and GpuFrameEnd marks the gap)
    if (!mark && !f.used && f.n) f.full = true;
    const int res = g.toResolve;
    if (!mark && res < 0) return false;
    ID3D12CommandList* all[72];
    UINT k = 0;
    // (perfDetail 8: once, before any list holding a pass timestamp, the list that writes them all)
    const bool prime = g.passOk && !g.primed;
    if (prime) all[k++] = g.passPrime;
    if (res >= 0) all[k++] = g.f[res].resolve[g.f[res].n / 2 - 1];
    // (perfDetail 8: that frame's pass timestamps, copied out by the submission our fence follows)
    if (res >= 0 && g.passOk && g.f[res].pn > 0) {
        all[k++] = g.f[res].passResolve;
        g.f[res].presolved = true;
    }
    if (mark) all[k++] = f.mark[f.n];
    for (UINT i = 0; i < n; ++i) all[k++] = lists[i];
    if (mark) all[k++] = f.mark[f.n + 1];
    real(q, k, all);
    if (prime) g.primed = true;
    if (res >= 0) {
        g.toResolve = -1;
        // (our fence right after the copy: that frame's timestamps can be read once it is reached)
        if (SUCCEEDED(q->Signal(g.fence, g.fenceValue + 1))) {
            g.f[res].ready = ++g.fenceValue;
        } else {
            g.ok = false;
            Log("Perf: GPU timestamps stopped (Signal failed)");
        }
    }
    if (mark) {
        f.ours[f.n / 2] = ours;
        f.n += 2;
    }
    return true;
}

void GpuFrameEnd()
{
    GpuState& g = g_gpu;
    if (!g.ok || !OnMain()) return;
    GpuFrame& f = g.f[g.cur];
    if (f.n > 0 && !f.used) {
        f.used = true;
        f.eye = g_gpuEye;
        f.gapBefore = g.gap;
        g.gap = false;
        g.toResolve = g.cur;
        g.cur = (g.cur + 1) % kGpuRing;
    } else {
        if (!f.used) {   // (perfDetail 8: this untimed frame's pass timestamps are dropped)
            f.pn = 0;
            f.pover = false;
        }
        g.gap = true;
    }
    GpuRead();
}

} // namespace Perf
