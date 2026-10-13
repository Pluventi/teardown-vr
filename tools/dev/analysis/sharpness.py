"""Sharpness (variance of the Laplacian) of eye captures, on scene regions.
usage: sharpness.py <bmp> [...]"""
import sys
import numpy as np
from PIL import Image

REGIONS = {
    "far (buildings)": (0.40, 0.58, 0.05, 0.55),
    "mid (garden)": (0.55, 0.70, 0.45, 0.95),
    "near (railing/floor)": (0.70, 0.95, 0.00, 0.35),
}
for f in sys.argv[1:]:
    a = np.asarray(Image.open(f).convert("L"), dtype=np.float32)
    h, w = a.shape
    out = []
    for name, (y0, y1, x0, x1) in REGIONS.items():
        r = a[int(h * y0):int(h * y1), int(w * x0):int(w * x1)]
        lap = -4 * r[1:-1, 1:-1] + r[:-2, 1:-1] + r[2:, 1:-1] + r[1:-1, :-2] + r[1:-1, 2:]
        out.append(f"{name}: {lap.var():.1f}")
    print(f.split("\\")[-1].split("/")[-1], " | ".join(out))
