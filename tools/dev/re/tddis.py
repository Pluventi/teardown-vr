"""Disassemble code of the running teardown.exe.
usage: tddis.py <rva hex> [length hex] [rva2 len2 ...]
Calls/jumps/rip-relative operands are shown as module RVAs; referenced
strings are printed when they look like text."""
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

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
args = sys.argv[1:]
i = 0
while i < len(args):
    rva = int(args[i], 16)
    length = int(args[i + 1], 16) if i + 1 < len(args) and not args[i + 1].startswith("@") else 0x200
    i += 2
    code = read(base + rva, length)
    print("==== rva %x" % rva)
    for ins in md.disasm(code, base + rva):
        ops = ins.op_str
        note = ""
        def rel(m):
            v = int(m.group(0), 16)
            if base <= v < base + 0x3000000:
                return "rva_%x" % (v - base)
            return m.group(0)
        ops2 = re.sub(r"0x[0-9a-f]{8,}", rel, ops)
        mm = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ops)
        if mm:
            off = int(mm.group(2), 16) * (1 if mm.group(1) == "+" else -1)
            target = ins.address + ins.size + off
            s = try_string(target)
            note = "  ; rva_%x" % (target - base) + (" \"%s\"" % s if s else "")
        print("  %x: %-6s %s%s" % (ins.address - base, ins.mnemonic, ops2, note))
        if ins.mnemonic == "int3":
            break
