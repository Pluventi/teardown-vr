"""List calls / strings / branches of a function in the running teardown.exe.
usage: calls.py <rva hex> [max bytes hex] [--all]"""
import ctypes, ctypes.wintypes as wt, sys, re
import psutil
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

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

def try_string(addr):
    b = read(addr, 80)
    m = re.match(rb"[\x20-\x7e]{4,}", b)
    return m.group().decode() if m else None

rva = int(sys.argv[1], 16)
length = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x2000
show_all = "--all" in sys.argv
code = read(base + rva, length)
md = Cs(CS_ARCH_X86, CS_MODE_64)
ret_count = 0
for ins in md.disasm(code, base + rva):
    a = ins.address - base
    ops = re.sub(r"0x[0-9a-f]{8,}", lambda m: ("rva_%x" % (int(m.group(0), 16) - base)) if base <= int(m.group(0), 16) < base + 0x3000000 else m.group(0), ins.op_str)
    note = ""
    mm = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
    if mm:
        off = int(mm.group(2), 16) * (1 if mm.group(1) == "+" else -1)
        t = ins.address + ins.size + off
        s = try_string(t)
        note = "  ; rva_%x" % (t - base) + (" \"%s\"" % s if s else "")
    if show_all or ins.mnemonic in ("call", "ret") or (note and "\"" in note) or "rva_1c6e270" in note:
        print("  %x: %-6s %s%s" % (a, ins.mnemonic, ops, note))
    if ins.mnemonic == "int3":
        ret_count += 1
        if ret_count >= 2:
            break
