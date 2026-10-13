"""Find data pointers (vtable slots) to given RVAs in the running teardown.exe
and print the surrounding vtable entries.  usage: vtref.py <rva hex> [...]"""
import ctypes, ctypes.wintypes as wt, os, sys, struct
import psutil, pefile

EXE = os.path.join(os.environ.get("TEARDOWN_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Teardown"), "teardown.exe")
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


pe = pefile.PE(EXE, fast_load=True)
for sec in pe.sections:
    if not sec.Name.startswith(b".rdata"):
        continue
    va, size = sec.VirtualAddress, sec.Misc_VirtualSize
    data = b""
    for off in range(0, size, 0x100000):
        data += read(base + va + off, min(0x100000, size - off))
    for a in sys.argv[1:]:
        ptr = struct.pack("<Q", base + int(a, 16))
        i = data.find(ptr)
        while i >= 0:
            print(f"{a} referenced at rdata {va + i:x}")
            for k in range(-90, -23):
                v = struct.unpack_from("<Q", data, i + k * 8)[0]
                print(f"   [{k:+d}] {va + i + k * 8:x}: {v - base:x}" if base <= v < base + 0x3000000 else f"   [{k:+d}] {v:x}")
            i = data.find(ptr, i + 1)
