"""Drive the mock headset through mock_out\\controls.txt (values persist between calls).
usage:
  mockctl.py point <x> <y> [click_s]   aim the right controller at a screen pixel (1920x1080), optional trigger click
  mockctl.py set name=value ...        set values (lx ly rx ry lt rt lg rg a b x menu lclick rclick aimyaw aimpitch
                                       headstill headpitch headyaw hx hy hz)
  mockctl.py hold name=value ... <s>   set values for <s> seconds, then reset them to 0
  mockctl.py snap [name]               capture the next headset frame, print the PNG path(s)
  mockctl.py pointinv <x> <y> <W> <H> [click_s]           aim at a pixel of the VR inventory panel (W x H px,
                                                          from the log line "Inventory: panel WxH px"), optional click.
                                                          The panel is worn at the belt: look at it first
                                                          ("set headpitch=-45") and raise the hand ("set hy=0.3 hz=0.1")
  mockctl.py draginv <x> <y0> <y1> <W> <H> [secs] [hold]  press at (x, y0), slide to y1 in secs, hold, release
  mockctl.py clear                     release everything (head stays still)
  mockctl.py pointpanel <u> <v> [r|l] [click_s]           aim a hand at canvas point (u, v) of a mod's menu shown as a
                                                          floating panel (last "mod panel ... anchor ... k ...; centre ...
                                                          normal ..." line of TeardownVR.log; recentre with the head still
                                                          first: set headstill=1 headyaw=0 headpitch=0, hold lclick=1 1.0)
"""
import glob, math, os, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
GAME = os.environ.get("TEARDOWN_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Teardown")
# the mock headset's output folder: TDVR_MOCK_OUT, else build\Release\mock_out in the repository
DIR = os.environ.get("TDVR_MOCK_OUT") or os.path.join(REPO, "build", "Release", "mock_out")
OUT = os.path.join(DIR, "controls.txt")
SCREEN_C = (-0.01, 1.65, -2.5)
SCREEN_W, SCREEN_H = 3.0, 3.0 * 1080 / 1920
HAND = (0.2, 1.25, -0.35)


def load():
    v = {}
    if os.path.exists(OUT):
        for line in open(OUT):
            if "=" in line:
                k, x = line.strip().split("=", 1)
                v[k] = float(x)
    v.setdefault("headstill", 1)
    return v


def save(v):
    tmp = OUT + ".tmp"
    with open(tmp, "w") as f:
        for k, x in v.items():
            f.write(f"{k}={x}\n")
    for _ in range(50):
        try:
            os.replace(tmp, OUT)
            return
        except PermissionError:
            time.sleep(0.005)


# Game back-buffer size (the 16:9 menus are letterboxed inside it).
BB_W, BB_H = int(os.environ.get("TD_BB_W", "1544")), int(os.environ.get("TD_BB_H", "1600"))


def aim(x, y):
    # x, y in 1920x1080 menu coordinates -> position on the back buffer (letterboxed 16:9 content)
    content_h = BB_W * 9 / 16
    u = x / 1920
    v = ((BB_H - content_h) / 2 + y / 1080 * content_h) / BB_H
    sw, sh = 3.0, 3.0 * BB_H / BB_W
    px = SCREEN_C[0] + (u - 0.5) * sw
    py = SCREEN_C[1] + (0.5 - v) * sh
    dx, dy, dz = px - HAND[0], py - HAND[1], SCREEN_C[2] - HAND[2]
    return {"aimyaw": round(math.degrees(math.atan2(-dx, -dz)), 3),
            "aimpitch": round(math.degrees(math.atan2(dy, math.hypot(dx, dz))), 3)}


def aim_ui(x, y):
    # settings panel: 1100x860 px, 1.0 m wide, centred 0.9 m ahead and 0.15 m below the (still) head
    w, h = 1.0, 1.0 * 860 / 1100
    px, py, pz = (x / 1100 - 0.5) * w, 1.5 + (0.5 - y / 860) * h, -0.9
    dx, dy, dz = px - HAND[0], py - HAND[1], pz - HAND[2]
    return {"aimyaw": round(math.degrees(math.atan2(-dx, -dz)), 3),
            "aimpitch": round(math.degrees(math.atan2(dy, math.hypot(dx, dz))), 3)}


def _rx(a, p):
    x, y, z = p
    return (x, y * math.cos(a) - z * math.sin(a), y * math.sin(a) + z * math.cos(a))


def _ry(a, p):
    x, y, z = p
    return (x * math.cos(a) + z * math.sin(a), y, -x * math.sin(a) + z * math.cos(a))


def _add(a, b):
    return tuple(i + j for i, j in zip(a, b))


def inv_point(x, y, w_px, h_px, v, scale=1.0, down=0.50, fwd=0.40):
    # VR inventory (vr.cpp UpdateInventoryPanel): 0.00058 m per pixel (x uiScale), worn at the belt,
    # `down` below and `fwd` ahead of the eyes (inventoryDown / inventoryForward), facing them. Neck
    # model: pivot 0.11 m below / 0.08 m behind the eyes. The still mock head is at (0, 1.65, 0),
    # turned by "headyaw" / "headpitch"; the body yaw is the head's (the panel was just shown).
    pitch, yaw = math.radians(v.get("headpitch", 0)), math.radians(v.get("headyaw", 0))
    e = (0.0, 0.11, -0.08)
    he = _ry(yaw, _rx(pitch, e))
    neck = (-he[0], 1.65 - he[1], -he[2])
    c = _add(_add(neck, _ry(yaw, e)), _ry(yaw, (0.0, -down, -fwd)))
    mpp = 0.00058 * scale
    lx, ly = (x - w_px / 2) * mpp, (h_px / 2 - y) * mpp
    return _add(c, _ry(yaw, _rx(-math.atan2(down, fwd), (lx, ly, 0.0))))


def aim_inv(x, y, w_px, h_px, v):
    px, py, pz = inv_point(x, y, w_px, h_px, v)
    # right hand: 0.2 m right, 0.4 m below, 0.35 m ahead of the still head, turned with "headyaw", + hx/hy/hz
    rel = _ry(math.radians(v.get("headyaw", 0)), (HAND[0], HAND[1] - 1.65, HAND[2]))
    hand = (rel[0] + v.get("hx", 0), 1.65 + rel[1] + v.get("hy", 0), rel[2] + v.get("hz", 0))
    dx, dy, dz = px - hand[0], py - hand[1], pz - hand[2]
    return {"aimyaw": round(math.degrees(math.atan2(-dx, -dz)), 3),
            "aimpitch": round(math.degrees(math.atan2(dy, math.hypot(dx, dz))), 3)}


GAME_LOG = os.path.join(GAME, "TeardownVR.log")


def panel_pose():
    # last logged pose of the floating panel (recentred headset space = stage space after a still-head recentre)
    import re
    rx = re.compile(r"mod panel .*?anchor (-?[\d.]+) (-?[\d.]+).*?k ([\d.]+); centre (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) "
                    r"normal (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")
    last = None
    for line in open(GAME_LOG, encoding="utf-8", errors="replace"):
        m = rx.search(line)
        if m: last = [float(x) for x in m.groups()]
    if not last: raise SystemExit("no 'mod panel ... centre' line in the log")
    return last


def aim_panel(u, w, hand, v):
    au, av, k, cx, cy, cz, nx, ny, nz = panel_pose()
    yaw, pitch = math.atan2(nx, nz), -math.asin(max(-1.0, min(1.0, ny)))   # the panel faces the eyes, upright
    lx, ly = (u - au) * k, -(w - av) * k
    px, py, pz = _add((cx, cy, cz), _ry(yaw, _rx(pitch, (lx, ly, 0.0))))
    side = 0.2 if hand == "r" else -0.2
    rel = _ry(math.radians(v.get("headyaw", 0)), (side, -0.4, -0.35))
    h = (rel[0], 1.65 + rel[1], rel[2])
    if hand == "r": h = (h[0] + v.get("hx", 0), h[1] + v.get("hy", 0), h[2] + v.get("hz", 0))
    dx, dy, dz = px - h[0], py - h[1], pz - h[2]
    a, b = ("aimyaw", "aimpitch") if hand == "r" else ("laimyaw", "laimpitch")
    return {a: round(math.degrees(math.atan2(-dx, -dz)), 3), b: round(math.degrees(math.atan2(dy, math.hypot(dx, dz))), 3)}


def kv(args):
    return {a.split("=")[0]: float(a.split("=")[1]) for a in args}


v = load()
cmd = sys.argv[1]
if cmd == "point":
    v.update(aim(float(sys.argv[2]), float(sys.argv[3])))
    save(v)
    time.sleep(0.6)
    if len(sys.argv) > 4:
        v["rt"] = 1; save(v); time.sleep(float(sys.argv[4]))
        v["rt"] = 0; save(v); time.sleep(0.3)
    print("aimed", v["aimyaw"], v["aimpitch"])
elif cmd == "pointui":   # pointui <x> <y> [click_s], panel pixels (1100x860)
    v.update(aim_ui(float(sys.argv[2]), float(sys.argv[3])))
    save(v)
    time.sleep(0.6)
    if len(sys.argv) > 4:
        v["rt"] = 1; save(v); time.sleep(float(sys.argv[4]))
        v["rt"] = 0; save(v); time.sleep(0.3)
    print("aimed ui", v["aimyaw"], v["aimpitch"])
elif cmd == "pointinv":   # pointinv <x> <y> <W> <H> [click_s]
    x, y, pw, ph = (float(a) for a in sys.argv[2:6])
    v.update(aim_inv(x, y, pw, ph, v))
    save(v)
    time.sleep(0.6)
    if len(sys.argv) > 6:
        v["rt"] = 1; save(v); time.sleep(float(sys.argv[6]))
        v["rt"] = 0; save(v); time.sleep(0.3)
    print("aimed inv", v["aimyaw"], v["aimpitch"])
elif cmd == "draginv":   # draginv <x> <y0> <y1> <W> <H> [secs] [hold_s]: fast slide + release = the list glides
    x, y0, y1, pw, ph = (float(a) for a in sys.argv[2:7])
    secs = float(sys.argv[7]) if len(sys.argv) > 7 else 0.4
    hold = float(sys.argv[8]) if len(sys.argv) > 8 else 0.0
    v.update(aim_inv(x, y0, pw, ph, v)); save(v); time.sleep(0.6)
    v["rt"] = 1; save(v); time.sleep(0.2)
    steps = max(2, int(secs / 0.02))
    for i in range(1, steps + 1):
        v.update(aim_inv(x, y0 + (y1 - y0) * i / steps, pw, ph, v)); save(v); time.sleep(secs / steps)
    time.sleep(hold)
    v["rt"] = 0; save(v); time.sleep(0.3)
    print("dragged inv", y0, "->", y1)
elif cmd == "pointpanel":   # pointpanel <u> <v> [r|l] [click_s]: canvas units of the mod's menu panel
    hand = sys.argv[4] if len(sys.argv) > 4 else "r"
    v.update(aim_panel(float(sys.argv[2]), float(sys.argv[3]), hand, v))
    save(v)
    time.sleep(0.6)
    if len(sys.argv) > 5:
        t = "rt" if hand == "r" else "lt"
        v[t] = 1; save(v); time.sleep(float(sys.argv[5]))
        v[t] = 0; save(v); time.sleep(0.3)
    print("aimed panel", {k: v[k] for k in ("aimyaw", "aimpitch", "laimyaw", "laimpitch") if k in v})
elif cmd == "set":
    v.update(kv(sys.argv[2:])); save(v); print("set", sys.argv[2:])
elif cmd == "hold":
    vals = kv(sys.argv[2:-1]); v.update(vals); save(v)
    time.sleep(float(sys.argv[-1]))
    for k in vals: v[k] = 0
    save(v); print("held", sys.argv[2:])
elif cmd == "snap":
    n = int(v.get("dump", 0)) + 1
    t_req = time.time()
    v["dump"] = n; save(v)
    time.sleep(1.5)
    from PIL import Image
    outs = []
    for f in sorted(glob.glob(os.path.join(DIR, f"{100 + n:02d}_*.bmp"))):
        if os.path.getmtime(f) < t_req - 0.5:
            continue   # stale capture from an earlier run
        im = Image.open(f)
        im.thumbnail((900, 900))
        png = f[:-4] + ".png"
        im.save(png)
        outs.append(png)
    if len(outs) == 2 and "eye" in outs[0]:   # side by side for the two eyes
        a, b = Image.open(outs[0]), Image.open(outs[1])
        sb = Image.new("RGB", (a.width + b.width + 8, max(a.height, b.height)))
        sb.paste(a, (0, 0)); sb.paste(b, (a.width + 8, 0))
        p = os.path.join(DIR, f"snap_{n}_eyes.png"); sb.save(p); outs.append(p)
    print("\n".join(outs) if outs else "no capture yet")
elif cmd == "clear":
    save({"headstill": 1, "dump": v.get("dump", 0)}); print("cleared")
