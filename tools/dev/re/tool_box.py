"""One-shot READ-ONLY bounding box of the held tool body's shapes (in the body's frame)."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tdmem as mem
def rot(q, v):
    x, y, z, w = q
    # v' = q v q*
    ix = w*v[0] + y*v[2] - z*v[1]; iy = w*v[1] + z*v[0] - x*v[2]; iz = w*v[2] + x*v[1] - y*v[0]; iw = -x*v[0] - y*v[1] - z*v[2]
    return (ix*w + iw*-x + iy*-z - iz*-y, iy*w + iw*-y + iz*-x - ix*-z, iz*w + iw*-z + ix*-y - iy*-x)
p = mem.player_entity()
ta = mem.u64(p + 0x990); entry = mem.u64(ta + 0x30)
print("tool", mem.cstr(entry + 0x18, 32) if entry else None)
body = mem.u64(p + 0x820)
c = mem.u64(body + 0x28); n = 0; lo = [1e9]*3; hi = [-1e9]*3; cnt = 0
while c and n < 64:
    ty = struct.unpack("<H", mem.read(c + 8, 2))[0]
    if ty == 2:
        lt = mem.fl(c + 0x44, 7); vd = mem.u64(c + 0xf0)
        sz = struct.unpack("<3i", mem.read(vd, 12)); sc = mem.f32(vd + 0x20)
        for k in range(8):
            cc = ((k & 1) * sz[0] * sc, ((k >> 1) & 1) * sz[1] * sc, ((k >> 2) & 1) * sz[2] * sc)
            r = rot(lt[3:7], cc); pp = [lt[i] + r[i] for i in range(3)]
            lo = [min(lo[i], pp[i]) for i in range(3)]; hi = [max(hi[i], pp[i]) for i in range(3)]
        cnt += 1
    c = mem.u64(c + 0x18); n += 1
print("shapes", cnt, "box", ["%.2f" % (hi[i] - lo[i]) for i in range(3)], "lo", ["%.2f" % v for v in lo], "hi", ["%.2f" % v for v in hi])
