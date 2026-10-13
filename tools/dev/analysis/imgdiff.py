# Image-equivalence check for frameBMode: snaps.json = [{"mode":N,"left":path,"right":path}, ...].
# Prints, per eye, the mean absolute difference between snaps of the SAME mode (noise) and between
# DIFFERENT modes, and the largest per-pixel-block differences.
import json, sys, itertools
import numpy as np
from PIL import Image

snaps = json.load(open(sys.argv[1]))
def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float32)
for eye in ('left', 'right'):
    imgs = [(s['mode'], load(s[eye])) for s in snaps]
    same, diff = [], []
    worst = (0, None)
    for (ma, a), (mb, b) in itertools.combinations(imgs, 2):
        if a.shape != b.shape:
            continue
        d = np.abs(a - b)
        m = float(d.mean())
        # 16x16 block maximum of the mean difference (localised changes: a missing line, a sprite)
        h, w = d.shape[0] // 16 * 16, d.shape[1] // 16 * 16
        blocks = d[:h, :w].mean(axis=2).reshape(h // 16, 16, w // 16, 16).mean(axis=(1, 3))
        bmax = float(blocks.max())
        (same if ma == mb else diff).append((m, bmax, ma, mb))
        if ma != mb and bmax > worst[0]:
            worst = (bmax, (ma, mb, np.unravel_index(int(blocks.argmax()), blocks.shape)))
    def stat(lst):
        if not lst:
            return 'n/a'
        ms = [x[0] for x in lst]; bs = [x[1] for x in lst]
        return f'mean {np.mean(ms):.3f} (max {np.max(ms):.3f}), block max {np.max(bs):.1f}'
    print(f'{eye}: same mode {stat(same)} | different modes {stat(diff)} | worst block {worst}')
