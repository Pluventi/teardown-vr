"""TAA captures vs a supersampled reference, aligned per region with sub-pixel shifts.
usage: taa_metrics2.py <dir> <name> ...  (cap_ref.bmp + cap_<name>.bmp)
detail = high-frequency energy relative to the reference (1 = same; <1 blur; >1 aliasing/oversharpening)
halo = mean overshoot (x1000) beyond the reference's local 3x3 min/max on strong edges"""
import sys, numpy as np
from PIL import Image
d = sys.argv[1]; names = sys.argv[2:]
def luma(im): a = np.asarray(im.convert('RGB'), dtype=np.float32) / 255.0; return 0.2126*a[...,0] + 0.7152*a[...,1] + 0.0722*a[...,2]
import glob, os
def files(n):
    fs = sorted(glob.glob(rf'{d}\cap_{n}_*.bmp'))
    return fs if fs else [rf'{d}\cap_{n}.bmp']
W, H = Image.open(files(names[0])[0]).size
REF = luma(Image.open(rf'{d}\cap_ref.bmp').convert('RGB').resize((W, H), Image.LANCZOS))
REG = {'forest far': (0.40, 0.50, 0.75, 0.98), 'buildings far': (0.40, 0.58, 0.05, 0.55), 'mid': (0.55, 0.70, 0.45, 0.95), 'near': (0.70, 0.95, 0.0, 0.35)}
def lap(a): return -4*a[1:-1,1:-1] + a[:-2,1:-1] + a[2:,1:-1] + a[1:-1,:-2] + a[1:-1,2:]
def subshift(a, b):
    h, w = a.shape; win = np.outer(np.hanning(h), np.hanning(w))
    F = np.fft.fft2((a - a.mean()) * win); G = np.fft.fft2((b - b.mean()) * win); R = F * np.conj(G); R /= np.abs(R) + 1e-9
    r = np.fft.ifft2(R).real; y0, x0 = np.unravel_index(np.argmax(r), r.shape)
    if y0 > h // 2: y0 -= h
    if x0 > w // 2: x0 -= w
    fy = np.fft.fftfreq(h)[:, None]; fx = np.fft.fftfreq(w)[None, :]; best = (-1e18, 0, 0)
    for dy in np.linspace(y0 - 1, y0 + 1, 21):
        for dx in np.linspace(x0 - 1, x0 + 1, 21):
            v = (R * np.exp(2j * np.pi * (fy * dy + fx * dx))).sum().real
            if v > best[0]: best = (v, dx, dy)
    return best[1], best[2]
def fshift(a, dx, dy):
    h, w = a.shape; fy = np.fft.fftfreq(h)[:, None]; fx = np.fft.fftfreq(w)[None, :]
    return np.fft.ifft2(np.fft.fft2(a) * np.exp(-2j * np.pi * (fy * dy + fx * dx))).real
def minmax3(a):
    p = np.pad(a, 1, mode='edge'); st = np.stack([p[y:y+a.shape[0], x:x+a.shape[1]] for y in range(3) for x in range(3)])
    return st.min(0), st.max(0)
for n in names:
  acc = {}
  for fpath in files(n):
    T = luma(Image.open(fpath))
    for rn, (y0, y1, x0, x1) in REG.items():
        # margin of 8 px so the shifted reference has valid content, measured inside
        Y0, Y1, X0, X1 = int(H*y0), int(H*y1), int(W*x0), int(W*x1)
        a = T[Y0:Y1, X0:X1]; b0 = REF[Y0:Y1, X0:X1]
        dx, dy = subshift(a, b0); b = fshift(b0, dx, dy)
        m = 8; a, b = a[m:-m, m:-m], b[m:-m, m:-m]
        mse = float(((a - b) ** 2).mean()); psnr = 10 * np.log10(1.0 / max(mse, 1e-12))
        sharp = float(lap(a).var() / max(lap(b).var(), 1e-12))
        mn, mx = minmax3(b); gy, gx = np.gradient(b); g = np.hypot(gx, gy); edge = g > np.percentile(g, 90)
        halo = float((np.maximum(0, a - mx) + np.maximum(0, mn - a))[edge].mean() * 1000)
        acc.setdefault(rn, []).append((psnr, sharp, halo))
  out = []
  for rn, v in acc.items():
    v = np.array(v); out.append(f"{rn}: {v[:,0].mean():.2f} dB detail x{v[:,1].mean():.2f} halo {v[:,2].mean():.2f}")
  print(f"{n:>5} ({len(files(n))}) | " + " | ".join(out))
