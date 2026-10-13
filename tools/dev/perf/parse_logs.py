"""Read-only: summarise the frame statistics of TeardownVR session logs.
usage: parse_logs.py <log> [<log> ...]"""
import re, sys, statistics as st

RS = re.compile(r"^\[\s*([\d.]+)\]\[\s*\d+\] Stereo frames: left=(\d+) right=(\d+) other=(\d+) \(game (\d+) fps, headset (\d+) fps\) "
                r"avg ms left=([\d.]+) right=([\d.]+) other=([\d.]+), xrWaitFrame ([\d.]+) ms x(\d+), worst frame ([\d.]+) ms, "
                r"(\d+) frames > 20 ms; slow game frames (\d+) \(worst ([\d.]+) ms\), long waits (\d+) \(worst ([\d.]+) ms\), "
                r"physics catch-ups dropped (\d+), headset refreshes missed (\d+) \((\d+) while")
RF = re.compile(r"^\[\s*([\d.]+)\]\[\s*\d+\] Slow frame ([\d.]+) ms \(eye (-?\d)\): update\+scripts ([\d.]+), render ([\d.]+), "
                r"VR submit ([\d.]+), present ([\d.]+), headset wait ([\d.]+) \(xrWaitFrame ([\d.]+)\), physics/other (-?[\d.]+)")
RG = re.compile(r"game time ([\d.]+) s for ([\d.]+) s of real time \(x([\d.]+)\)")

def pct(v, p):
    if not v: return 0
    v = sorted(v); return v[min(len(v) - 1, int(p * len(v)))]

for path in sys.argv[1:]:
    stats, slow, gt = [], [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = RS.search(line)
        if m:
            g = m.groups(); stats.append(dict(t=float(g[0]), L=int(g[1]), R=int(g[2]), O=int(g[3]), gfps=int(g[4]), hfps=int(g[5]),
                msL=float(g[6]), msR=float(g[7]), msO=float(g[8]), wait=float(g[9]), worst=float(g[11]), late=int(g[12]),
                slowW=int(g[13]), worstW=float(g[14]), lw=int(g[15]), lwmax=float(g[16]), drops=int(g[17]), miss=int(g[18]), missrt=int(g[19])))
            continue
        m = RF.search(line)
        if m:
            g = m.groups(); slow.append(dict(t=float(g[0]), f=float(g[1]), eye=int(g[2]), upd=float(g[3]), ren=float(g[4]), sub=float(g[5]),
                pre=float(g[6]), hw=float(g[7]), xw=float(g[8]), oth=float(g[9])))
            continue
        m = RG.search(line)
        if m: gt.append(float(m.group(3)))
    st_s = [s for s in stats if s["L"] > 0]
    print(f"=== {path.split('/')[-1].split(chr(92))[-1]}: {len(stats)} stats lines ({len(st_s)} in stereo), {len(slow)} slow-frame lines")
    if st_s:
        dur = sum(s['L'] + s['R'] for s in st_s)
        for k, lab in (("msL", "avg ms A(left)"), ("msR", "avg ms B(right)"), ("wait", "xrWaitFrame avg")):
            v = [s[k] for s in st_s]; print(f"  {lab:18s} median {st.median(v):5.1f}  p90 {pct(v,.9):5.1f}  max {max(v):5.1f}")
        print(f"  game fps median {st.median([s['gfps'] for s in st_s])}  headset fps median {st.median([s['hfps'] for s in st_s])}")
        print(f"  worst frame per 600: median {st.median([s['worst'] for s in st_s]):.1f} max {max(s['worst'] for s in st_s):.1f}; "
              f"frames>20ms total {sum(s['late'] for s in st_s)} over {dur} frames ({100*sum(s['late'] for s in st_s)/max(1,dur):.2f}%)")
        print(f"  slow game work (>14ms) total {sum(s['slowW'] for s in st_s)}  worst work max {max(s['worstW'] for s in st_s):.1f}")
        print(f"  long waits {sum(s['lw'] for s in st_s)} (worst {max(s['lwmax'] for s in st_s):.1f}); refreshes missed {sum(s['miss'] for s in st_s)} "
              f"({sum(s['missrt'] for s in st_s)} runtime-held); catch-ups dropped last {st_s[-1]['drops']}")
        # windows with the heaviest A frames
        top = sorted(st_s, key=lambda s: -s['msL'])[:5]
        print("  heaviest windows (t, msA, msB, worst, >20, slowW, worstW, missed, drops):")
        for s in top:
            print(f"    t={s['t']:8.1f} A={s['msL']:5.1f} B={s['msR']:5.1f} worst={s['worst']:6.1f} >20={s['late']:3d} slowW={s['slowW']:3d} worstW={s['worstW']:6.1f} miss={s['miss']:3d} drops={s['drops']}")
    sf = [s for s in slow if s["eye"] >= 0]
    if sf:
        for e in (0, 1):
            v = [s for s in sf if s["eye"] == e]
            if not v: continue
            print(f"  slow frames eye {e} ({'A' if e==0 else 'B'}): n={len(v)}  f median {st.median([x['f'] for x in v]):.1f} max {max(x['f'] for x in v):.1f} | "
                  f"upd med {st.median([x['upd'] for x in v]):.1f} max {max(x['upd'] for x in v):.1f} | ren med {st.median([x['ren'] for x in v]):.1f} max {max(x['ren'] for x in v):.1f} | "
                  f"sub med {st.median([x['sub'] for x in v]):.1f} max {max(x['sub'] for x in v):.1f} | phys/oth med {st.median([x['oth'] for x in v]):.1f} max {max(x['oth'] for x in v):.1f} | "
                  f"hw med {st.median([x['hw'] for x in v]):.1f}")
        # dominant component per slow frame
        dom = {}
        for s in sf:
            k = max(("upd", "ren", "sub", "pre", "hw", "oth"), key=lambda k: s[k]); dom[k] = dom.get(k, 0) + 1
        print("  dominant component of slow stereo frames:", dom)
        print("  worst 8 slow stereo frames:")
        for s in sorted(sf, key=lambda s: -s['f'])[:8]:
            print(f"    t={s['t']:8.1f} {s['f']:6.1f} ms eye {s['eye']}: upd {s['upd']:5.1f} ren {s['ren']:5.1f} sub {s['sub']:5.1f} pre {s['pre']:4.1f} hw {s['hw']:5.1f} phys/oth {s['oth']:6.1f}")
    if gt: print(f"  game-time ratio: median x{st.median(gt):.2f} min x{min(gt):.2f}")
