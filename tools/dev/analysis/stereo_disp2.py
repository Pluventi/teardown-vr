"""Offset of a region of the left eye in the right eye (same stereo pair), by normalised
cross-correlation at half resolution, sub-pixel refined. Regions in full-res pixels.
usage: stereo_disp2.py <left.bmp> <right.bmp> x0 y0 x1 y1 [x0 y0 x1 y1 ...]
prints dx dy (full-res px) per region"""
import sys
import numpy as np
from PIL import Image

S = 2
L = np.asarray(Image.open(sys.argv[1]).convert('L'), dtype=np.float32)
R = np.asarray(Image.open(sys.argv[2]).convert('L'), dtype=np.float32)
def half(a):
    h, w = a.shape[0] // S * S, a.shape[1] // S * S
    return a[:h, :w].reshape(h // S, S, w // S, S).mean(axis=(1, 3))
L, R = half(L), half(R)
args = list(map(int, sys.argv[3:]))
for k in range(0, len(args), 4):
    x0, y0, x1, y1 = [v // S for v in args[k:k + 4]]
    t = L[y0:y1, x0:x1]
    t = (t - t.mean()) / (t.std() + 1e-6)
    best = None
    scores = {}
    for dy in range(-30, 31):
        for dx in range(-420, -120):
            if y0 + dy < 0 or x0 + dx < 0 or y1 + dy > R.shape[0] or x1 + dx > R.shape[1]:
                continue
            c = R[y0 + dy:y1 + dy, x0 + dx:x1 + dx]
            c = (c - c.mean()) / (c.std() + 1e-6)
            s = (t * c).mean()
            scores[(dx, dy)] = s
            if best is None or s > best[0]:
                best = (s, dx, dy)
    s, dx, dy = best
    # parabolic sub-pixel refinement
    def sub(a, b, c):
        d = a - 2 * b + c
        return 0.0 if abs(d) < 1e-9 else 0.5 * (a - c) / d
    fx = sub(scores.get((dx - 1, dy), s), s, scores.get((dx + 1, dy), s))
    fy = sub(scores.get((dx, dy - 1), s), s, scores.get((dx, dy + 1), s))
    print("region %s: dx %.2f dy %.2f (ncc %.3f)" % (args[k:k + 4], (dx + fx) * S, (dy + fy) * S, s))
