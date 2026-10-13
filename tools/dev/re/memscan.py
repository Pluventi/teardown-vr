"""Read teardown.exe's decrypted code from the running process and find
references to given strings, then disassemble around them."""
import ctypes, ctypes.wintypes as wt, os, sys, struct
import psutil, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = os.path.join(os.environ.get("TEARDOWN_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Teardown"), "teardown.exe")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

def open_proc():
    pid = next(p.pid for p in psutil.process_iter(["name"]) if p.info["name"] and p.info["name"].lower() == "teardown.exe")
    h = k32.OpenProcess(0x0410, False, pid)
    mods = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
    return h, mods[0]

def read(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n))
    return buf.raw[:n.value]

def main():
    h, base = open_proc()
    pe = pefile.PE(EXE, fast_load=True)
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    code = read(h, base + text.VirtualAddress, text.Misc_VirtualSize)
    print("base %x text rva %x size %x read %x" % (base, text.VirtualAddress, text.Misc_VirtualSize, len(code)))
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for target_str in sys.argv[1:]:
        off = pe.__data__.find(target_str.encode() + b"\0")
        rva = pe.get_rva_from_offset(off)
        va = base + rva
        print("== %r at rva %x" % (target_str, rva))
        tbase = base + text.VirtualAddress
        # scan for rip-relative disp32 references
        for i in range(len(code) - 7):
            if code[i + 1] != 0x8D:
                continue
            if code[i] not in (0x48, 0x4C):
                continue
            modrm = code[i + 2]
            if (modrm & 0xC7) != 0x05:
                continue
            disp = struct.unpack_from("<i", code, i + 3)[0]
            if tbase + i + 7 + disp == va:
                ref = tbase + i
                print("  xref at rva %x" % (ref - base))
                start = max(0, i - 0x60)
                for ins in md.disasm(code[start:i + 0x40], tbase + start):
                    mark = "  <==" if ins.address == ref else ""
                    print("    %x: %s %s%s" % (ins.address - base, ins.mnemonic, ins.op_str, mark))

main()
