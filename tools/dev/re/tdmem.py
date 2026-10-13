"""Read-only memory helper for the running teardown.exe (ReadProcessMemory only). Offsets: Teardown 2.0.1."""
import ctypes, ctypes.wintypes as wt, struct, re
import psutil
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
pid = next(p.pid for p in psutil.process_iter(["name"]) if p.info["name"] and p.info["name"].lower() == "teardown.exe")
h = k32.OpenProcess(0x0410, False, pid)
mods = (ctypes.c_void_p * 1024)()
needed = wt.DWORD()
psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
base = mods[0]
def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n))
    return buf.raw[:n.value]
def u64(a): return struct.unpack("<Q", read(a, 8))[0]
def i32(a): return struct.unpack("<i", read(a, 4))[0]
def f32(a): return struct.unpack("<f", read(a, 4))[0]
def fl(a, n): return struct.unpack("<%df" % n, read(a, 4 * n))
def cstr(a, n=80):
    b = read(a, n); m = re.match(rb"[\x20-\x7e]*", b); return m.group().decode()
def game(): return u64(base + 0x1c6e270)
def player_entity():
    g = game(); arr = u64(g + 0xc0); idx = i32(g + 0x168); return u64(arr + 8 * idx)
