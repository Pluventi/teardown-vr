"""Per-thread CPU of teardown.exe, sampled from OUTSIDE the game (read-only, no debugger,
nothing injected, no thread suspended): psutil thread times + QueryThreadCycleTime (cycle-accurate) + each
thread's start address (NtQueryInformationThread class 9) mapped to its module. Also tails TeardownVR.log so the
samples can be put on the log's clock (both are QPC: offset = perf_counter - log_t, constant over the run).

usage: thread_sampler.py <pid> <out.csv> <TeardownVR.log> <seconds> [interval_s=0.5]
csv: perf, wall, newest_log_t, tid, start, user_s, kernel_s, cycles
"""
import csv, ctypes, ctypes.wintypes as wt, re, sys, time
import psutil

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
THREAD_QUERY_INFORMATION = 0x0040
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
RX = re.compile(rb"^\[\s*([\d.]+)\]\[", re.M)   # (the line time, not the thread id that follows it)


class MODULEINFO(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]


def modules(pid):
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    arr = (ctypes.c_void_p * 2048)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 3)
    out = []
    for i in range(min(need.value // 8, 2048)):
        mi = MODULEINFO()
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleInformation(h, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi))
        psapi.GetModuleBaseNameW(h, ctypes.c_void_p(arr[i]), name, 260)
        out.append((mi.base or 0, mi.size, name.value))
    k32.CloseHandle(h)
    return out


def where(addr, mods):
    for b, s, n in mods:
        if b <= addr < b + s:
            return "%s+%x" % (n, addr - b)   # (our DLL is dsound.dll; job workers share one teardown.exe+X)
    return hex(addr)


def newest_log_t(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            n = f.tell()
            f.seek(max(0, n - 4096))
            ts = RX.findall(f.read())
            return float(ts[-1]) if ts else ""
    except OSError:
        return ""


def main():
    pid, out, logp, secs = int(sys.argv[1]), sys.argv[2], sys.argv[3], float(sys.argv[4])
    dt = float(sys.argv[5]) if len(sys.argv) > 5 else 0.5
    proc = psutil.Process(pid)
    mods = modules(pid)
    handles, starts = {}, {}
    w = csv.writer(open(out, "w", newline=""))
    w.writerow(["perf", "wall", "newest_log_t", "tid", "start", "user_s", "kernel_s", "cycles"])
    t_end = time.perf_counter() + secs
    nxt = time.perf_counter()
    while time.perf_counter() < t_end and proc.is_running():
        nxt += dt
        perf, wall, lt = time.perf_counter(), time.time(), newest_log_t(logp)
        for th in proc.threads():
            tid = th.id
            if tid not in handles:
                h = k32.OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                handles[tid] = h
                a = ctypes.c_void_p()
                if h and ntdll.NtQueryInformationThread(h, 9, ctypes.byref(a), 8, None) == 0:
                    starts[tid] = where(a.value or 0, mods)
                else:
                    starts[tid] = "?"
            c = ctypes.c_ulonglong()
            ok = bool(handles[tid]) and k32.QueryThreadCycleTime(handles[tid], ctypes.byref(c))
            cyc = c.value if ok else ""
            w.writerow(["%.4f" % perf, "%.3f" % wall, lt, tid, starts[tid], "%.4f" % th.user_time, "%.4f" % th.system_time, cyc])
        time.sleep(max(0.0, nxt - time.perf_counter()))
    for h in handles.values():
        if h:
            k32.CloseHandle(h)
    # cycles -> seconds: calibrate with this thread spinning for 1 s
    me = k32.GetCurrentThread()
    c0, c1 = ctypes.c_ulonglong(), ctypes.c_ulonglong()
    k32.QueryThreadCycleTime(me, ctypes.byref(c0))
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 1.0:
        pass
    k32.QueryThreadCycleTime(me, ctypes.byref(c1))
    w.writerow(["#cycles_per_s", (c1.value - c0.value) / (time.perf_counter() - t0), "", "", "", "", "", ""])


if __name__ == "__main__":
    main()
