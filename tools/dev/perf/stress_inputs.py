"""Fixed, timestamped input schedule for the mock headset stress test (one process, so the
cadence does not depend on Python start-up times). Writes mock_out\\controls.txt exactly like mockctl.py.

Phases: CALM (no input) -> INFLICT (rockets along a fixed fan of aims, optional mock Lua explosions) -> AFTER (no
input: debris settles, fires burn - the "it freezes after I shot everywhere" part).
The head stays still (headstill=1): the camera is fixed; only the right hand aims (aimyaw/aimpitch).

usage: stress_inputs.py --calm 20 --inflict 40 --after 40 --mode both --out phases.json --log <TeardownVR.log>
"""
import argparse, json, os, re, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
# the mock headset's output folder: TDVR_MOCK_OUT, else build\Release\mock_out in the repository
DIR = os.environ.get("TDVR_MOCK_OUT") or os.path.join(REPO, "build", "Release", "mock_out")
OUT = os.path.join(DIR, "controls.txt")
# Aim fan (degrees, stage space; 0/0 = straight ahead of the still head). Chosen ONCE from the "stress_view" snap so
# every rocket hits a structure >= 10 m away (never the ground near the player: explosions hurt and push him).
# Freeze the list after that first check: it is part of the test definition.
AIMS = [(-35, -3), (-20, 3), (-5, -5), (10, 4), (25, -2), (38, 6), (30, -7), (15, 0), (0, 8), (-15, -1), (-28, 5), (-40, -6)]
SHOT_EVERY = 1.0        # s between rockets (above the launcher's reload time)
TRIGGER_HOLD = 0.12     # s
BOOM_EVERY = 4          # every 4th shot also a mock Lua Explosion (size 4, 7 m ahead of the head; needs offGrab=0)
BOOM_HOLD = 0.08        # short: the left grip also crouches the player while held (camera dips)
RX = re.compile(rb"^\[\s*([\d.]+)\]\[", re.M)   # (the line time, not the thread id that follows it)


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


def log_time(path):
    """Timestamp of the newest TeardownVR.log line (QPC seconds since the DLL loaded; perf_counter is QPC too)."""
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            n = f.tell()
            f.seek(max(0, n - 8192))
            ts = RX.findall(f.read())
            return float(ts[-1]) if ts else None
    except OSError:
        return None


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--calm", type=float, default=20)
    a.add_argument("--inflict", type=float, default=40)
    a.add_argument("--after", type=float, default=40)
    a.add_argument("--mode", default="both")
    a.add_argument("--out", required=True)
    a.add_argument("--log", required=True)
    o = a.parse_args()
    v = load()
    v.update(headstill=1, headyaw=0, headpitch=0, rt=0, lg=0, lt=0, rg=0)
    save(v)
    rec = {"mode": o.mode, "aims": AIMS, "phases": [], "actions": []}

    def mark(name):
        rec["phases"].append({"name": name, "perf": time.perf_counter(), "wall": time.time(), "log_t": log_time(o.log)})

    def act(what, **kw):
        v.update(kw)
        save(v)
        rec["actions"].append({"t": time.perf_counter(), "what": what, **kw})

    mark("CALM")
    time.sleep(o.calm)
    mark("INFLICT")
    t0 = time.perf_counter()
    shot = 0
    while time.perf_counter() - t0 < o.inflict:
        due = t0 + shot * SHOT_EVERY
        while time.perf_counter() < due:
            time.sleep(0.002)
        yaw, pitch = AIMS[shot % len(AIMS)]
        act("aim", aimyaw=yaw, aimpitch=pitch)
        time.sleep(0.25)                                   # the laser / player orientation settle on the target
        if o.mode in ("rocket", "both"):
            act("fire", rt=1)
            time.sleep(TRIGGER_HOLD)
            act("release", rt=0)
        if o.mode == "boom" or (o.mode == "both" and shot % BOOM_EVERY == BOOM_EVERY - 1):
            time.sleep(0.2)
            act("boom", lg=1)
            time.sleep(BOOM_HOLD)
            act("boom_release", lg=0)
        shot += 1
    act("rest", aimyaw=0, aimpitch=0, rt=0, lg=0)
    mark("AFTER")
    time.sleep(o.after)
    mark("END")
    rec["shots"] = shot
    json.dump(rec, open(o.out, "w"), indent=1)
    print("shots", shot, "phases", [(p["name"], p["log_t"]) for p in rec["phases"]])


if __name__ == "__main__":
    main()
