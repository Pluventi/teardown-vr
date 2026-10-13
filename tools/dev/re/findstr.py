"""Find text in the running teardown.exe image (all sections, read-only).
usage: findstr.py [--ci] <text> [<text> ...]      prints the RVA of every match
       findstr.py --dump <rva hex> [len hex]       prints the text stored at an RVA (e.g. a string the code refers to)"""
import ctypes, ctypes.wintypes as wt, os, sys
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


args = sys.argv[1:]
if args and args[0] == "--dump":
    rva = int(args[1], 16)
    length = int(args[2], 16) if len(args) > 2 else 0x1000
    data = read(base + rva, length)
    print(data.split(b"\0", 1)[0].decode("latin-1"))
    sys.exit(0)

ci = "--ci" in args
needles = [a for a in args if a != "--ci"]
pe = pefile.PE(EXE, fast_load=True)
for sec in pe.sections:
    va, size = sec.VirtualAddress, sec.Misc_VirtualSize
    data = b"".join(read(base + va + o, min(0x100000, size - o)) for o in range(0, size, 0x100000))
    hay = data.lower() if ci else data
    for n in needles:
        nb = n.encode("latin-1")
        if ci:
            nb = nb.lower()
        i = hay.find(nb)
        count = 0
        while i >= 0 and count < 200:
            s = i
            while s > 0 and 0x20 <= data[s - 1] < 0x7f and i - s < 120:
                s -= 1
            e = data.find(b"\0", i)
            text = data[s:min(e if e >= 0 else i + 120, i + 160)].decode("latin-1", "replace").replace("\n", "\\n")
            print(f"{sec.Name.rstrip(bytes(1)).decode()} rva {va + i:x} (string starts {va + s:x}): {text}")
            count += 1
            i = hay.find(nb, i + 1)
