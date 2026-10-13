#include "log.h"
#include "config.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

// Lines are formatted on the calling thread (time, thread id, text, CRLF - the same bytes the
// old text-mode fprintf wrote) and appended to g_pend. The writer thread swaps g_pend out and
// writes it in one WriteFile every 50 ms (sooner past 64 KB). The old logger did fprintf +
// fflush on the calling thread for every line: one WriteFile on the game's main thread per
// line (10-30 us, and rare multi-millisecond stalls with antivirus scanning).
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
LARGE_INTEGER g_start, g_freq;
// g_pendLock: the lines waiting (held only to append or swap). g_writeLock: held while a batch
// is written, so that batches reach the file in order (writer thread, LogFlush).
SRWLOCK g_pendLock = SRWLOCK_INIT;
SRWLOCK g_writeLock = SRWLOCK_INIT;
std::string g_pend;                     // (capacity reserved once: no allocation per line)
std::string g_out;                      // the batch being written (g_writeLock)
std::atomic<size_t> g_pendBytes{ 0 };
std::atomic<bool> g_async{ false };
std::atomic<bool> g_stop{ false };
std::atomic<unsigned> g_dropped{ 0 };   // lines dropped: more than kMaxPending waiting (disk stalled)
HANDLE g_wake = nullptr;                // auto-reset: write now
constexpr size_t kReserve = 256 * 1024;
constexpr size_t kWakeAt = 64 * 1024;
constexpr size_t kMaxPending = 8 * 1024 * 1024;
constexpr DWORD kPeriodMs = 50;
constexpr DWORD kBoundMs = 100;         // LogFlush, LogShutdown: at most this long for each lock

double Seconds()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
}

void WriteAll(const char* p, size_t n)
{
    while (n > 0) {
        DWORD w = 0;
        if (!WriteFile(g_file, p, (DWORD)std::min<size_t>(n, 1u << 20), &w, nullptr) || w == 0) return;
        p += w;
        n -= w;
    }
}

// The lock: INFINITE waits for it; otherwise at most maxMs of real time, 0 = only if it is free
// now (a thread killed or suspended while it held it never lets go: SRW locks are not reentrant
// either, so a crash inside Log on this thread ends here too).
bool LockWithin(SRWLOCK* l, DWORD maxMs)
{
    if (maxMs == INFINITE) {
        AcquireSRWLockExclusive(l);
        return true;
    }
    if (TryAcquireSRWLockExclusive(l)) return true;
    // (measured, not counted in Sleep(1)s: without timeBeginPeriod one lasts ~15.6 ms)
    const ULONGLONG end = GetTickCount64() + maxMs;
    while (GetTickCount64() < end) {
        Sleep(1);
        if (TryAcquireSRWLockExclusive(l)) return true;
    }
    return false;
}

// Writes the lines waiting (g_writeLock held by the caller), waiting at most maxMs for the
// lines' lock (see LockWithin). false: that lock was busy, nothing written.
bool WritePending(DWORD maxMs)
{
    if (!LockWithin(&g_pendLock, maxMs)) return false;
    g_out.swap(g_pend);
    g_pendBytes = 0;
    const unsigned dropped = g_dropped.exchange(0);
    ReleaseSRWLockExclusive(&g_pendLock);
    if (!g_out.empty()) WriteAll(g_out.data(), g_out.size());
    g_out.clear();
    if (dropped) {
        char line[160];
        int n = snprintf(line, sizeof(line), "[%9.3f][%5lu] Log: %u lines dropped (the disk did not keep up)\r\n", Seconds(),
                         GetCurrentThreadId(), dropped);
        if (n > 0) WriteAll(line, (size_t)std::min(n, (int)sizeof(line) - 1));
    }
    return true;
}

// Writes the lines waiting, waiting at most maxMs for each lock (see LockWithin). false: a lock
// was busy, nothing written.
bool Flush(DWORD maxMs)
{
    if (g_file == INVALID_HANDLE_VALUE) return false;
    if (!LockWithin(&g_writeLock, maxMs)) return false;
    const bool ok = WritePending(maxMs);
    ReleaseSRWLockExclusive(&g_writeLock);
    return ok;
}

DWORD WINAPI WriterThread(LPVOID)
{
    while (!g_stop) {
        WaitForSingleObject(g_wake, kPeriodMs);
        if (g_stop) break;
        if (g_pendBytes.load(std::memory_order_relaxed) == 0) continue;
        Flush(INFINITE);
    }
    return 0;
}

// A fatal-kind exception. Vectored handlers see it at first chance, before any __try that
// handles it (ours, the game's, a driver's or an overlay's probing memory): it may not be a crash,
// and the thread may be the game's main thread. So never wait here: the lines are written only
// if both locks are free right now (at most one WriteFile per batch of lines - never more than
// the old line-by-line writes); if the writer thread is writing, it is woken to write the rest
// right after (in a real crash it keeps running while the process is reported and ended). Never
// logs, never touches anything else.
LONG CALLBACK CrashFlush(EXCEPTION_POINTERS* ep)
{
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000374:   // STATUS_HEAP_CORRUPTION
    case 0xC0000409:   // STATUS_STACK_BUFFER_OVERRUN
        // (not a stack overflow: too little stack left to write)
        if (g_async && g_pendBytes.load(std::memory_order_relaxed) > 0 && !Flush(0)) SetEvent(g_wake);
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The previous session's log (ended normally or by a crash) is kept in TeardownVR_journaux\ next to the game, named
// after its last write (local time), with the sessions before it - at most kKeepLogs: what a player sends to report a
// problem (the installer's "Préparer mes journaux" button zips them). Moved, not copied: this runs in DllMain, so
// plain file calls only. Busy (another program holds it without delete sharing): overwritten, as before.
constexpr size_t kKeepLogs = 10;

void ArchivePreviousLog(const std::wstring& dir)
{
    const std::wstring cur = dir + L"TeardownVR.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(cur.c_str(), GetFileExInfoStandard, &fa) || (fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0))
        return;
    const std::wstring arch = dir + L"TeardownVR_journaux\\";
    CreateDirectoryW(arch.c_str(), nullptr);
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &st)) return;
    wchar_t name[64];
    swprintf_s(name, L"TeardownVR_%04u-%02u-%02u_%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    std::wstring dest = arch + name + L".log";
    for (int k = 2; k < 100 && GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES; ++k)
        dest = arch + name + L"_" + std::to_wstring(k) + L".log";
    if (!MoveFileExW(cur.c_str(), dest.c_str(), 0)) return;
    // (the names sort by date: the oldest ones first)
    std::vector<std::wstring> logs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((arch + L"TeardownVR_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) logs.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + kKeepLogs < logs.size(); ++i) DeleteFileW((arch + logs[i]).c_str());
}

} // namespace

void LogInit()
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    ArchivePreviousLog(ModDirectory());
    std::wstring path = ModDirectory() + L"TeardownVR.log";
    // (others may read it while the game runs, not write: as the old _wfsopen "w", _SH_DENYWR)
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    g_pend.reserve(kReserve);
    g_out.reserve(kReserve);
}

void LogConfigure(bool async)
{
    if (g_file == INVALID_HANDLE_VALUE || !async || g_async) return;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) return;
    // (created from DllMain: the thread starts once the loader lock is released; nothing waits for it)
    HANDLE t = CreateThread(nullptr, 64 * 1024, WriterThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return;
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);   // (it only writes; the game threads come first)
    CloseHandle(t);
    AddVectoredExceptionHandler(0, CrashFlush);           // (last: after every other handler)
    g_async = true;
}

void Log(const char* fmt, ...)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    const double t = Seconds();

    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    // "[time][tid] text" + CRLF, embedded line feeds as CRLF too (what the text-mode FILE wrote).
    char line[64 + 2 * sizeof(text) + 2];
    int len = snprintf(line, 64, "[%9.3f][%5lu] ", t, GetCurrentThreadId());
    if (len < 0 || len >= 64) len = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') line[len++] = '\r';
        line[len++] = *p;
    }
    line[len++] = '\r';
    line[len++] = '\n';

    if (!g_async) {
        // Synchronous (logSync=1, or before the ini is read): written by this thread, in order.
        AcquireSRWLockExclusive(&g_writeLock);
        WritePending(INFINITE);   // (lines appended before the switch, if any)
        WriteAll(line, (size_t)len);
        ReleaseSRWLockExclusive(&g_writeLock);
        return;
    }
    size_t waiting;
    AcquireSRWLockExclusive(&g_pendLock);
    if (g_pend.size() + (size_t)len > kMaxPending) {
        ++g_dropped;
        waiting = g_pend.size();
    } else {
        g_pend.append(line, (size_t)len);
        waiting = g_pend.size();
        g_pendBytes = waiting;
    }
    ReleaseSRWLockExclusive(&g_pendLock);
    if (waiting >= kWakeAt) SetEvent(g_wake);
}

void LogFlush()
{
    // (bounded, 0.1 s per lock: called from the hang detector and stack dumps, which must never
    // hang on the log)
    Flush(kBoundMs);
}

void LogShutdown()
{
    g_stop = true;
    // The writer thread is not waited for (DllMain: the loader lock; at process exit it is
    // already gone, maybe while holding a lock - hence the bounded flush).
    Flush(kBoundMs);
}

void LogStack(const char* tag)
{
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    auto base = (ULONG_PTR)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    ULONG_PTR end = base + nt->OptionalHeader.SizeOfImage;
    char line[1024];
    int len = snprintf(line, sizeof(line), "Stack %s:", tag);
    for (USHORT i = 0; i < n && len < (int)sizeof(line) - 16; ++i) {
        auto a = (ULONG_PTR)frames[i];
        if (a >= base && a < end) len += snprintf(line + len, sizeof(line) - len, " %llx", (unsigned long long)(a - base));
        else len += snprintf(line + len, sizeof(line) - len, " ?");
    }
    Log("%s", line);
    LogFlush();
}
