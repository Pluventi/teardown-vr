"""Read-only analysis of a stress run (folder written by stress_run.ps1) or of any TeardownVR.log.
usage: analyze_run.py <run folder | TeardownVR.log> [--phases t0,t1,t2,t3]   (log-time phase bounds for a bare log)
       analyze_run.py --compare <runA1,runA2,..> <runB1,runB2,..>
Per phase (CALM / INFLICT / AFTER): work per A and B frame, pair work vs the 90 Hz budget, B share, hitches,
headset refreshes missed per second (app vs runtime-held), physics catch-ups dropped per second, slow-frame
breakdown per eye (mock BMP-dump stalls removed), Lua physics steps/frame, game-time ratio, shapes, and - when
present - per-thread CPU (main thread / job workers / rest), CPU and GPU counters."""
import csv, json, os, re, statistics as st, sys
from collections import defaultdict

BUDGET = 1000.0 / 90
RS = re.compile(r"^\[\s*([\d.]+)\]\[\s*(\d+)\] Stereo frames: left=(\d+) right=(\d+) other=(\d+) \(game (\d+) fps, headset (\d+) fps\) "
                r"avg ms left=([\d.]+) right=([\d.]+) other=([\d.]+), xrWaitFrame ([\d.]+) ms x(\d+), worst frame ([\d.]+) ms, "
                r"(\d+) frames > 20 ms; slow game frames (\d+) \(worst ([\d.]+) ms\), long waits (\d+) \(worst ([\d.]+) ms\), "
                r"physics catch-ups dropped (\d+), headset refreshes missed (\d+) \((\d+) while")
RF = re.compile(r"^\[\s*([\d.]+)\]\[\s*\d+\] Slow frame ([\d.]+) ms \(eye (-?\d)\): update\+scripts ([\d.]+), render ([\d.]+), "
                r"VR submit ([\d.]+), present ([\d.]+), headset wait ([\d.]+) \(xrWaitFrame ([\d.]+)\), physics/other (-?[\d.]+)")
RSTEP = re.compile(r"^\[\s*([\d.]+)\].*Lua: phys contact .*steps/frame ([\d.]+)")
RGT = re.compile(r"^\[\s*([\d.]+)\].*Engine: game time ([\d.]+) s for ([\d.]+) s")
RSH = re.compile(r"^\[\s*([\d.]+)\].*right-eye object motion: (\d+) shapes")
RBLK = re.compile(r"^\[\s*([\d.]+)\].*XR: xrEndFrame blocked ([\d.]+) ms")
RPERF = re.compile(r"^\[\s*([\d.]+)\].*perf A: frame ([\d.]+) = update ([\d.]+), fixed loop ([\d.]+).*?\| perf B: frame ([\d.]+) = update ([\d.]+), fixed loop ([\d.]+).*?rest ([\d.]+)")
RCAM = re.compile(r"^\[\s*([\d.]+)\].*camera motion over 300 pairs: mean step ([\d.]+) mm")


def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float("nan")


def med(v):
    return st.median(v) if v else float("nan")


def parse_log(path):
    d = defaultdict(list)
    prev_drop = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = RS.search(line)
        if m:
            mp = RPERF.search(line)
            if mp:
                d["perf"].append(tuple(float(x) for x in mp.groups()))
            g = m.groups()
            t, L, R = float(g[0]), int(g[2]), int(g[3])
            drops = int(g[18])
            dd = drops - prev_drop if prev_drop is not None else 0
            prev_drop = drops
            if L == 0:
                continue
            msL, msR, wait = float(g[7]), float(g[8]), float(g[10])
            dur = (L * msL + R * msR + int(g[4]) * float(g[9])) / 1000.0
            d["win"].append(dict(t=t, t0=t - dur, dur=dur, tid=int(g[1]), wA=msL - wait, wB=msR, wait=wait,
                                 late=int(g[13]), slow=int(g[14]), worst=float(g[15]), miss=int(g[19]),
                                 missrt=int(g[20]), drops=dd, gfps=int(g[5]), hfps=int(g[6])))
            continue
        m = RF.search(line)
        if m:
            g = [float(x) for x in m.groups()]
            d["slow"].append(dict(t=g[0], f=g[1], eye=int(g[2]), upd=g[3], ren=g[4], sub=g[5], pre=g[6], hw=g[7], oth=g[9]))
            continue
        for key, rx in (("steps", RSTEP), ("gt", RGT), ("shapes", RSH), ("blk", RBLK), ("cam", RCAM), ("perf", RPERF)):
            m = rx.search(line)
            if m:
                d[key].append(tuple(float(x) for x in m.groups()))
                break
    return d


def phase_of(t, bounds):
    for name, a, b in bounds:
        if a <= t < b:
            return name
    return None


def summarize(d, bounds, threads=None):
    rows = {}
    blk = [b[0] for b in d["blk"]]
    for name, a, b in bounds:
        W = [w for w in d["win"] if a <= (w["t0"] + w["t"]) / 2 < b]
        S = [s for s in d["slow"] if a <= s["t"] < b and s["eye"] >= 0 and s["sub"] < 20 and not any(abs(s["t"] - x) < 0.2 for x in blk)]
        r = {"windows": len(W)}
        if W:
            dur = sum(w["dur"] for w in W)
            r.update(
                workA_med=med([w["wA"] for w in W]), workB_med=med([w["wB"] for w in W]),
                pair_med=med([w["wA"] + w["wB"] for w in W]), pair_p90=q([w["wA"] + w["wB"] for w in W], .9),
                pair_max=max(w["wA"] + w["wB"] for w in W),
                budget_use_med=med([(w["wA"] + w["wB"]) / BUDGET for w in W]),
                B_share_med=med([w["wB"] / max(1e-6, w["wA"] + w["wB"]) for w in W]),
                hitch_per_600=med([w["late"] for w in W]), worst_frame=max(w["worst"] for w in W),
                missed_app_per_s=sum(w["miss"] - w["missrt"] for w in W) / dur,
                missed_rt_per_s=sum(w["missrt"] for w in W) / dur,
                drops_per_s=sum(w["drops"] for w in W) / dur, game_fps_med=med([w["gfps"] for w in W]))
        for e, lab in ((0, "A"), (1, "B")):
            E = [s for s in S if s["eye"] == e]
            if E:
                r["slow%s_n" % lab] = len(E)
                r["slow%s_upd_med" % lab] = med([s["upd"] for s in E])
                r["slow%s_phys_med" % lab] = med([s["oth"] for s in E])
                r["slow%s_ren_med" % lab] = med([s["ren"] for s in E])
                r["freezes%s_gt50" % lab] = sum(1 for s in E if s["f"] - s["hw"] > 50)
        stp = [x[1] for x in d["steps"] if a <= x[0] < b]
        if stp:
            r["steps_per_frame_med"], r["steps_per_frame_max"] = med(stp), max(stp)
        gt = [x[1] / x[2] for x in d["gt"] if a <= x[0] < b]
        if gt:
            r["game_time_ratio_min"] = min(gt)
        sh = [x[1] for x in d["shapes"] if x[0] < b]
        if sh:
            r["shapes_last"] = sh[-1]
        cm = [x[1] for x in d["cam"] if a <= x[0] < b]
        if cm:
            r["camera_mean_step_mm_max"] = max(cm)   # head is still: > ~2 mm = the camera moved (knock-back, death)
        pf = [x for x in d["perf"] if a <= x[0] < b]
        if pf:   # perfTrace: per-eye update (all mods' tick + game update) and frame A's physics loop
            r["perfA_update_med"], r["perfA_fixed_med"] = med([x[2] for x in pf]), med([x[3] for x in pf])
            r["perfB_update_med"], r["perfB_update_max"] = med([x[5] for x in pf]), max(x[5] for x in pf)
            r["perf_cpu_pair_med"] = med([x[2] + x[3] + x[5] + x[6] for x in pf])
        if threads:
            r.update(threads(a, b))
        rows[name] = r
    return rows


def thread_summary(path, main_tid, d):
    rows = list(csv.reader(open(path)))
    cps = next((float(r[1]) for r in rows if r and r[0] == "#cycles_per_s"), None)
    rows = [r for r in rows[1:] if r and not r[0].startswith("#")]
    # perf -> log time: perf_counter and the log are both QPC; offset = min(perf - newest line time)
    off = min(float(r[0]) - float(r[2]) for r in rows if r[2])
    by_t = defaultdict(dict)
    for r in rows:
        by_t[float(r[0]) - off][int(r[3])] = (r[4], float(r[5]) + float(r[6]), int(r[7]) if r[7] else None)
    ts = sorted(by_t)

    def f(a, b):
        sel = [t for t in ts if a <= t <= b]
        if len(sel) < 2:
            return {}
        t0, t1 = sel[0], sel[-1]
        span = t1 - t0
        cpu = {}
        for tid, (start, s1, c1) in by_t[t1].items():
            if tid in by_t[t0]:
                s0, c0 = by_t[t0][tid][1], by_t[t0][tid][2]
                busy = (c1 - c0) / cps if (cps and c1 is not None and c0 is not None) else (s1 - s0)
                cpu[tid] = (start, busy / span)
        groups = defaultdict(float)
        starts = defaultdict(int)
        for tid, (start, u) in cpu.items():
            starts[start] += 1
        worker_start = max((s for s in starts if s.startswith("teardown.exe")), key=lambda s: starts[s], default=None)
        for tid, (start, u) in cpu.items():
            g = "main" if tid == main_tid else ("workers" if start == worker_start else
                ("ours(dsound.dll)" if start.startswith("dsound.dll") else start.split("+")[0]))
            groups[g] += u
        top = sorted(groups.items(), key=lambda kv: -kv[1])[:6]
        return {"cpu_cores_" + k: round(v, 2) for k, v in top} | {"worker_threads": starts.get(worker_start, 0)}
    return f


def run(path, bounds_arg=None):
    folder = path if os.path.isdir(path) else None
    logp = os.path.join(folder, "TeardownVR.log") if folder else path
    d = parse_log(logp)
    if folder and os.path.exists(os.path.join(folder, "phases.json")):
        ph = json.load(open(os.path.join(folder, "phases.json")))["phases"]
        bounds = [(ph[i]["name"], ph[i]["log_t"], ph[i + 1]["log_t"]) for i in range(len(ph) - 1)]
    elif bounds_arg:
        t = [float(x) for x in bounds_arg.split(",")]
        bounds = list(zip(["CALM", "INFLICT", "AFTER", "P4", "P5"], t[:-1], t[1:]))
    else:
        bounds = [("WHOLE", 60.0, 1e9)]   # (the first minute: loading spikes of 400-600 ms)
    thr = None
    if folder and os.path.exists(os.path.join(folder, "threads.csv")) and d["win"]:
        thr = thread_summary(os.path.join(folder, "threads.csv"), d["win"][0]["tid"], d)
    return summarize(d, bounds, thr)


def show(res):
    keys = []
    for r in res.values():
        keys += [k for k in r if k not in keys]
    print("%-26s" % "metric" + "".join("%12s" % n for n in res))
    for k in keys:
        vals = [res[n].get(k) for n in res]
        print("%-26s" % k + "".join(("%12.2f" % v if isinstance(v, float) else "%12s" % ("" if v is None else v)) for v in vals))


if __name__ == "__main__":
    if sys.argv[1] == "--compare":
        A = [run(p) for p in sys.argv[2].split(",")]
        B = [run(p) for p in sys.argv[3].split(",")]
        for ph in A[0]:
            print("== phase", ph)
            for k in A[0][ph]:
                va = [r[ph].get(k) for r in A if isinstance(r.get(ph, {}).get(k), (int, float))]
                vb = [r[ph].get(k) for r in B if isinstance(r.get(ph, {}).get(k), (int, float))]
                if va and vb:
                    print("  %-26s A med %8.2f [%7.2f..%7.2f]   B med %8.2f [%7.2f..%7.2f]   delta %+8.2f" % (
                        k, med(va), min(va), max(va), med(vb), min(vb), max(vb), med(vb) - med(va)))
    else:
        show(run(sys.argv[1], sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--phases" else None))
