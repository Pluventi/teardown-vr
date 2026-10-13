"""Find instructions in teardown.exe .text whose operands contain given
substrings (e.g. '+ 0x2cc]'). Prints rva + instruction + enclosing function
start (approximated by the last int3 padding)."""
import ctypes, ctypes.wintypes as wt, os, sys, struct
import psutil, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

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
code = read(base + text.VirtualAddress, text.Misc_VirtualSize)
patterns = sys.argv[1:]
# quick byte prefilter on the displacement values
disps = []
for p in patterns:
    import re
    m = re.search(r"0x([0-9a-f]+)\]", p)
    if m:
        disps.append(struct.pack("<I", int(m.group(1), 16)))
md = Cs(CS_ARCH_X86, CS_MODE_64)
seen = set()
for d in disps:
    start = 0
    while True:
        i = code.find(d, start)
        if i < 0:
            break
        start = i + 1
        # try decoding instructions that could contain this displacement
        for back in range(2, 9):
            off = i - back
            if off < 0:
                continue
            for ins in md.disasm(code[off:off + 16], base + text.VirtualAddress + off, count=1):
                if ins.size >= back + 4 and any(p in ins.op_str for p in patterns):
                    rva = ins.address - base
                    if rva in seen:
                        continue
                    seen.add(rva)
                    # function start: scan back for cc cc padding
                    j = off
                    while j > 0 and not (code[j - 1] == 0xCC and code[j - 2] == 0xCC):
                        j -= 1
                    print("%x: %s %s   (fn ~%x)" % (rva, ins.mnemonic, ins.op_str, j + text.VirtualAddress))
