"""Print raw bytes of the running teardown.exe: bytes.py <rva hex> <len hex> [...]"""
import ctypes, ctypes.wintypes as wt, sys
import psutil
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
pid = next(p.pid for p in psutil.process_iter(["name"]) if p.info["name"] and p.info["name"].lower() == "teardown.exe")
h = k32.OpenProcess(0x0410, False, pid)
mods = (ctypes.c_void_p * 1024)()
needed = wt.DWORD()
psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
base = mods[0]
a = sys.argv[1:]
for i in range(0, len(a), 2):
    rva, n = int(a[i], 16), int(a[i + 1], 16)
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(base + rva), buf, n, ctypes.byref(got))
    print("%x: %s" % (rva, " ".join("%02X" % b for b in buf.raw[:got.value])))
