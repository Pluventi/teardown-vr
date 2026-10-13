"""Sample RIP + a few stack return addresses of every thread of teardown.exe
(module-relative when inside teardown.exe or our dsound.dll)."""
import ctypes, ctypes.wintypes as wt, struct
import psutil

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P1Home", ctypes.c_uint64), ("P2Home", ctypes.c_uint64), ("P3Home", ctypes.c_uint64),
                ("P4Home", ctypes.c_uint64), ("P5Home", ctypes.c_uint64), ("P6Home", ctypes.c_uint64),
                ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD), ("SegGs", wt.WORD),
                ("SegSs", wt.WORD), ("EFlags", wt.DWORD),
                ("Dr0", ctypes.c_uint64), ("Dr1", ctypes.c_uint64), ("Dr2", ctypes.c_uint64), ("Dr3", ctypes.c_uint64),
                ("Dr6", ctypes.c_uint64), ("Dr7", ctypes.c_uint64),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64), ("Rbx", ctypes.c_uint64),
                ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64), ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64),
                ("R8", ctypes.c_uint64), ("R9", ctypes.c_uint64), ("R10", ctypes.c_uint64), ("R11", ctypes.c_uint64),
                ("R12", ctypes.c_uint64), ("R13", ctypes.c_uint64), ("R14", ctypes.c_uint64), ("R15", ctypes.c_uint64),
                ("Rip", ctypes.c_uint64), ("rest", ctypes.c_byte * 1232)]

proc = next(p for p in psutil.process_iter(["name"]) if p.info["name"] and p.info["name"].lower() == "teardown.exe")
h = k32.OpenProcess(0x1F0FFF, False, proc.pid)
mods = (ctypes.c_void_p * 1024)()
needed = wt.DWORD()
psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
ranges = []
for m in mods[: needed.value // 8]:
    if not m:
        continue
    name = ctypes.create_unicode_buffer(260)
    psapi.GetModuleBaseNameW(h, ctypes.c_void_p(m), name, 260)
    class MI(ctypes.Structure):
        _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]
    mi = MI()
    psapi.GetModuleInformation(h, ctypes.c_void_p(m), ctypes.byref(mi), ctypes.sizeof(mi))
    ranges.append((m, m + mi.size, name.value))

def sym(a):
    for lo, hi, n in ranges:
        if lo <= a < hi:
            return "%s+%x" % (n, a - lo)
    return None

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n))
    return buf.raw[:n.value]

main_tid = min(t.id for t in proc.threads())
for t in proc.threads()[:80]:
    th = k32.OpenThread(0x005A, False, t.id)  # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
    if not th:
        continue
    k32.SuspendThread(th)
    ctx = CONTEXT()
    ctx.ContextFlags = 0x100003  # CONTEXT_CONTROL | INTEGER (AMD64)
    ok = k32.GetThreadContext(th, ctypes.byref(ctx))
    k32.ResumeThread(th)
    k32.CloseHandle(th)
    if not ok:
        continue
    stack = read(ctx.Rsp, 0x1000)
    rets = []
    for i in range(0, len(stack) - 8, 8):
        v = struct.unpack_from("<Q", stack, i)[0]
        s = sym(v)
        if s and ("teardown.exe" in s or "dsound.dll" in s):
            rets.append(s.replace("teardown.exe", "TD").replace("dsound.dll", "VR"))
        if len(rets) >= 10:
            break
    rip = sym(ctx.Rip) or hex(ctx.Rip)
    if any("TD+" in r for r in rets) or "teardown" in rip:
        print("tid %5d rip %-28s %s" % (t.id, rip, " ".join(rets)))
