# Pause menu (in a level): the bottom bar's LAST button (Natural Phenomena) on a quad0 capture.
# Prints the centre of its label as fractions u v of the virtual screen (bright label pixels,
# grouped into runs; the last run is the last button's label).
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
w, h = im.size
px = im.load()
y = int(0.974 * h)
xs = [x for x in range(w) if max(sum(px[x, yy]) for yy in range(y - 3, y + 4)) > 3 * 200]
if not xs:
    print('none'); sys.exit(1)
runs, cur = [], [xs[0]]
for x in xs[1:]:
    if x - cur[-1] > 0.03 * w:
        runs.append(cur); cur = [x]
    else:
        cur.append(x)
runs.append(cur)
last = runs[-1]
print(round((min(last) + max(last)) / 2 / w, 4), round(y / h, 4))
