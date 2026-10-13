"""Find code in the running teardown.exe that calls given imports.
usage: iatrefs.py <dll> <func> [<func> ...]"""
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
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
want = {f.encode() for f in sys.argv[2:]}
slots = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    if d.dll.decode().lower() != sys.argv[1].lower():
        continue
    for imp in d.imports:
        if imp.name in want:
            slots[imp.address - pe.OPTIONAL_HEADER.ImageBase] = imp.name.decode()
print("slots", {hex(k): v for k, v in slots.items()})

text = next(s for s in pe.sections if s.Name.startswith(b".text"))
tva, tsize = text.VirtualAddress, text.Misc_VirtualSize
code = b""
for off in range(0, tsize, 0x100000):
    code += read(base + tva + off, min(0x100000, tsize - off))
for i in range(len(code) - 6):
    if code[i] == 0xFF and code[i + 1] in (0x15, 0x25):
        disp = struct.unpack_from("<i", code, i + 2)[0]
        target = tva + i + 6 + disp
        if target in slots:
            # function start: last int3 before
            j = i
            while j > 0 and not (code[j - 1] == 0xCC and code[j - 2] == 0xCC):
                j -= 1
            print(f"{slots[target]:20s} call at {tva + i:x}  (function ~{tva + j:x})")
