"""Compile an HLSL file with Teardown's OWN shader compiler (dxcompiler.dll + dxil.dll of the game folder,
DXC 1.8.2403 - the version the game's shader cache was built with). Read-only for the game.
usage: gamedxc.py <file.hlsl> <entry> <profile> <out.cso> [-DNAME=VALUE ...] [--dis out.txt] [--header out.h NAME]
The output is a signed DXIL container (dxil.dll validates and signs it, as dxc.exe does)."""
import ctypes
import os
import sys
from ctypes import c_void_p, c_uint32, c_wchar_p, POINTER, byref, HRESULT, c_size_t

GAME = os.environ.get("TEARDOWN_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Teardown")


class GUID(ctypes.Structure):
    _fields_ = [("a", c_uint32), ("b", ctypes.c_uint16), ("c", ctypes.c_uint16), ("d", ctypes.c_ubyte * 8)]


def guid(s):
    import uuid
    g = GUID()
    ctypes.memmove(byref(g), uuid.UUID(s).bytes_le, 16)
    return g


class DxcDefine(ctypes.Structure):
    _fields_ = [("Name", c_wchar_p), ("Value", c_wchar_p)]


def vcall(obj, idx, restype, *args, argtypes=()):
    vt = ctypes.cast(ctypes.cast(obj, POINTER(c_void_p))[0], POINTER(c_void_p))
    return ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)(vt[idx])(obj, *args)


def blob_bytes(b):
    p = vcall(b, 3, c_void_p)
    n = vcall(b, 4, c_size_t)
    return ctypes.string_at(p, n)


_dll = None


def instances():
    global _dll
    if _dll is None:
        ctypes.WinDLL(os.path.join(GAME, "dxil.dll"))          # the validator (signs the container)
        _dll = ctypes.WinDLL(os.path.join(GAME, "dxcompiler.dll"))
    f = _dll.DxcCreateInstance
    f.argtypes = [POINTER(GUID), POINTER(GUID), POINTER(c_void_p)]
    comp, lib = c_void_p(), c_void_p()
    assert f(byref(guid("73e22d93-e6ce-47f3-b5bf-f0664f39c1b0")), byref(guid("8c210bf3-011f-4422-8d70-6f9acb8db617")),
             byref(comp)) == 0
    assert f(byref(guid("6245d6af-66e0-48fd-80b4-4d271796748c")), byref(guid("e5204dc7-d18c-4c3c-bdfb-851673980fe7")),
             byref(lib)) == 0
    return comp, lib


def compile_hlsl(path, entry, profile, defines, extra_args=()):
    comp, lib = instances()
    src = open(path, "rb").read()
    buf = ctypes.create_string_buffer(src, len(src))
    blob = c_void_p()
    hr = vcall(lib, 6, HRESULT, buf, c_uint32(len(src)), c_uint32(65001), byref(blob),
               argtypes=(c_void_p, c_uint32, c_uint32, POINTER(c_void_p)))
    assert hr == 0, hex(hr)
    defs = (DxcDefine * max(1, len(defines)))(*[DxcDefine(k, v) for k, v in defines])
    args = (c_wchar_p * max(1, len(extra_args)))(*extra_args)
    res = c_void_p()
    hr = vcall(comp, 3, HRESULT, blob, os.path.basename(path), entry, profile, args, c_uint32(len(extra_args)), defs,
               c_uint32(len(defines)), None, byref(res),
               argtypes=(c_void_p, c_wchar_p, c_wchar_p, c_wchar_p, POINTER(c_wchar_p), c_uint32, POINTER(DxcDefine),
                         c_uint32, c_void_p, POINTER(c_void_p)))
    assert hr == 0, hex(hr)
    status = ctypes.c_long()
    vcall(res, 3, HRESULT, byref(status), argtypes=(POINTER(ctypes.c_long),))
    err = c_void_p()
    vcall(res, 5, HRESULT, byref(err), argtypes=(POINTER(c_void_p),))
    msg = blob_bytes(err).decode("utf-8", "replace") if err else ""
    if status.value != 0:
        raise SystemExit("compile failed:\n" + msg)
    out = c_void_p()
    vcall(res, 4, HRESULT, byref(out), argtypes=(POINTER(c_void_p),))
    return blob_bytes(out), msg


def disassemble(data):
    comp, lib = instances()
    buf = ctypes.create_string_buffer(data, len(data))
    blob = c_void_p()
    vcall(lib, 6, HRESULT, buf, c_uint32(len(data)), c_uint32(0), byref(blob),
          argtypes=(c_void_p, c_uint32, c_uint32, POINTER(c_void_p)))
    out = c_void_p()
    vcall(comp, 5, HRESULT, blob, byref(out), argtypes=(c_void_p, POINTER(c_void_p)))
    return blob_bytes(out).decode("latin-1")


def header(data, name, comment):
    lines = ["// " + l for l in comment.splitlines()]
    lines.append("const unsigned char %s[] = {" % name)
    for i in range(0, len(data), 12):
        lines.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ("," if i + 12 < len(data) else ""))
    lines.append("};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    path, entry, profile, out = sys.argv[1:5]
    defines, dis, hdr = [], None, None
    rest = sys.argv[5:]
    i = 0
    while i < len(rest):
        a = rest[i]
        if a.startswith("-D"):
            k, _, v = a[2:].partition("=")
            defines.append((k, v or "1"))
        elif a == "--dis":
            dis = rest[i + 1]
            i += 1
        elif a == "--header":
            hdr = (rest[i + 1], rest[i + 2])
            i += 2
        i += 1
    data, msg = compile_hlsl(path, entry, profile, defines)
    if msg.strip():
        print(msg.strip())
    assert data[:4] == b"DXBC"
    assert any(data[4:20]), "container not signed (dxil.dll missing?)"
    open(out, "wb").write(data)
    print(out, len(data), "bytes, hash", data[4:20].hex())
    if dis:
        open(dis, "w", encoding="latin-1").write(disassemble(data))
    if hdr:
        defs = " ".join("%s=%s" % d for d in defines)
        open(hdr[0], "w", newline="\r\n").write(header(
            data, hdr[1], "Generated by tools/dev/shader_tools/gamedxc.py (Teardown's own dxcompiler.dll 1.8.2403, %s) from\n"
            "shaders/%s with %s.\nContainer hash %s, %d bytes. Do not edit: rebuild with the same command."
            % (profile, os.path.basename(path), defs, data[4:20].hex(), len(data))))
