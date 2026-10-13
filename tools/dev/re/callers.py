"""Find direct calls/jumps (E8/E9 rel32) and RIP-relative references to given RVAs
in the running teardown.exe.  usage: callers.py <rva hex> [...]"""
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
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
tva, tsize = text.VirtualAddress, text.Misc_VirtualSize
code = b""
for off in range(0, tsize, 0x100000):
    code += read(base + tva + off, min(0x100000, tsize - off))
targets = {int(a, 16) for a in sys.argv[1:]}
for i in range(len(code) - 5):
    op = code[i]
    if op in (0xE8, 0xE9):
        t = tva + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        if t in targets:
            print(f"{'call' if op == 0xE8 else 'jmp '} {t:x} from {tva + i:x}")
    # lea reg,[rip+X] (48/4C 8D modrm with mod=00 rm=101)
    if op in (0x48, 0x4C) and code[i + 1] == 0x8D and (code[i + 2] & 0xC7) == 0x05 and i + 7 <= len(code):
        t = tva + i + 7 + struct.unpack_from("<i", code, i + 3)[0]
        if t in targets:
            print(f"lea  {t:x} at {tva + i:x}")
