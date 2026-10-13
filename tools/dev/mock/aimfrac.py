# Aim the mock's right hand at a fraction (u, v) of the virtual screen whose aspect is W/H
# (in a level the pause menu fills the whole screen: canvas 1920 x 1888 -> aspect 1920/1888), then
# optionally click. usage: aimfrac.py <u> <v> [aspect] [click_s]
import math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
spec = importlib.util.spec_from_file_location('mockctl_mod', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mockctl.py'))
src = open(spec.origin, encoding='utf-8').read().split('\nv = load()')[0]   # (definitions only)
ns = {'__file__': spec.origin}   # (mockctl.py finds the repository from its own path)
exec(compile(src, 'mockctl_defs', 'exec'), ns)
u, v = float(sys.argv[1]), float(sys.argv[2])
aspect = float(sys.argv[3]) if len(sys.argv) > 3 else 1920 / 1888
sw = 3.0
sh = sw / aspect
C, H = ns['SCREEN_C'], ns['HAND']
px, py, pz = C[0] + (u - 0.5) * sw, C[1] + (0.5 - v) * sh, C[2]
dx, dy, dz = px - H[0], py - H[1], pz - H[2]
vals = ns['load']()
vals['aimyaw'] = round(math.degrees(math.atan2(-dx, -dz)), 3)
vals['aimpitch'] = round(math.degrees(math.atan2(dy, math.hypot(dx, dz))), 3)
ns['save'](vals)
time.sleep(0.6)
if len(sys.argv) > 4:
    vals['rt'] = 1; ns['save'](vals); time.sleep(float(sys.argv[4]))
    vals['rt'] = 0; ns['save'](vals); time.sleep(0.3)
print('aimed', vals['aimyaw'], vals['aimpitch'])
