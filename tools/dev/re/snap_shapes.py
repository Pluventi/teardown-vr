"""One-shot READ-ONLY dump of the held tool body's shapes (local transform, size, voxel scale)."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdmem as mem, struct
p = mem.player_entity()
ta = mem.u64(p + 0x990); entry = mem.u64(ta + 0x30)
print("tool", mem.cstr(entry + 0x18, 32) if entry else None)
body = mem.u64(p + 0x820)
print("body %x type %d handle %d" % (body, struct.unpack("<H", mem.read(body + 8, 2))[0], mem.i32(body + 0xc)))
c = mem.u64(body + 0x28); n = 0
while c and n < 40:
    ty = struct.unpack("<H", mem.read(c + 8, 2))[0]
    if ty == 2:
        lt = mem.fl(c + 0x44, 7); vd = mem.u64(c + 0xf0)
        sz = struct.unpack("<3i", mem.read(vd, 12)) if vd else (0, 0, 0); sc = mem.f32(vd + 0x20) if vd else 0
        print(" shape h%d pos %s rot %s size %s scale %.4f" % (mem.i32(c + 0xc), ["%.3f" % v for v in lt[:3]], ["%.3f" % v for v in lt[3:]], sz, sc))
    c = mem.u64(c + 0x18); n += 1
