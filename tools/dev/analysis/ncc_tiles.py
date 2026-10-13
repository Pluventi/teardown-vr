"""Stereo grain metric: for each 48 px tile of the left eye, the best normalised cross-correlation in the right
eye (and its offset), against the score at exactly -512 px. The tile grid and the search window fit one test
scene: adjust them to yours.
usage: ncc_tiles.py <folder> <name> [<name> ...]   (reads <folder>/<name>_left.bmp and <folder>/<name>_right.bmp)"""
import os, sys, numpy as np
from PIL import Image
from scipy.ndimage import uniform_filter, convolve
from scipy.signal import fftconvolve
W=os.path.join(sys.argv[1], '')
K=np.array([[0,1,0],[1,4,1],[0,1,0]],np.float32)/8
def lum(p):
    a=np.asarray(Image.open(p),dtype=np.float32)/255
    return a@np.array([.2126,.7152,.0722],np.float32)
def prep(a): a=convolve(a,K); return a-uniform_filter(a,7)
def ncc(l, R):
    l=l-l.mean(); n=l.size
    num=fftconvolve(R, l[::-1,::-1], mode='valid')
    s=fftconvolve(R, np.ones_like(l), mode='valid'); s2=fftconvolve(R*R, np.ones_like(l), mode='valid')
    return num/np.sqrt(np.maximum(s2-s*s/n,1e-9)*(l*l).sum())
T=48; DXA,DXB,DY=-660,-480,3
for k in sys.argv[2:]:
    L=prep(lum(W+k+'_left.bmp')); R=prep(lum(W+k+'_right.bmp'))
    best=[];offs=[];at512=[]
    for y in range(1380,1980,T):
        for x in range(1900,2500,T):
            l=L[y:y+T,x:x+T]
            c=ncc(l,R[y-DY:y+T+DY, x+DXA:x+T+DXB])
            j,i=np.unravel_index(np.argmax(c),c.shape)
            best.append(c[j,i]); offs.append(DXA+i); at512.append(c[DY, -512-DXA])
    best=np.array(best); offs=np.array(offs); at512=np.array(at512)
    good=best>0.25
    print('%-26s tiles %d: best median %.3f, >0.25 in %d%% (offsets of those: median %d, 10-90%% %d..%d) | at exactly -512: median %.3f'
          %(k,len(best),np.median(best),100*good.mean(),np.median(offs[good]) if good.any() else 0,
            np.percentile(offs[good],10) if good.any() else 0,np.percentile(offs[good],90) if good.any() else 0,np.median(at512)))
# noise floor: the left image of the first snapshot against the right image of the last one (needs 2 names)
if len(sys.argv) < 4: sys.exit(0)
L=prep(lum(W+sys.argv[2]+'_left.bmp')); R=prep(lum(W+sys.argv[-1]+'_right.bmp'))
b=[]
for y in range(1380,1980,T):
    for x in range(1900,2500,T):
        c=ncc(L[y:y+T,x:x+T],R[y-DY:y+T+DY, x+DXA:x+T+DXB]); b.append(c.max())
print('noise floor (unrelated snapshots): best median %.3f, >0.25 in %d%%'%(np.median(b),100*(np.array(b)>0.25).mean()))
