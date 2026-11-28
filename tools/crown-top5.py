#!/usr/bin/env python3
"""crown-top5.py IMAGE X Y W H [LOD.png] [label] (from the light lane's top5.py, branch far-light): OKLCh L, C, h of the brightest 5 % (by OKLab L) of crown pixels in the
rect (crowns: the lod view's non-sky, non-terrain pixels when LOD.png is given; else grass and sky left out by
colour: L > 0.45 and hue < 128, or hue 200-300 with L > 0.6), and of the median band (47.5-52.5 %) for contrast."""
import sys
import numpy as np
from PIL import Image
M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566], [0.0883024619, 0.2817188376, 0.6299787005]])
M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099], [0.0259040371, 0.7827717662, -0.8086757660]])
f = sys.argv[1]; x, y, w, h = map(int, sys.argv[2:6]); lod = sys.argv[6] if len(sys.argv) > 6 and sys.argv[6].endswith('.png') else None
label = sys.argv[-1] if not sys.argv[-1].endswith('.png') and len(sys.argv) > 6 else f
c = np.asarray(Image.open(f).convert('RGB'), np.float64)[y:y + h, x:x + w] / 255
lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
lab = np.cbrt(lin @ M1.T) @ M2.T; L = lab[..., 0]; hue = np.degrees(np.arctan2(lab[..., 2], lab[..., 1])) % 360
if lod:
    d = np.asarray(Image.open(lod).convert('RGB'), np.float64)[y:y + h, x:x + w] / 255
    m = (d.max(-1) > 0.05) & (d.max(-1) - d.min(-1) > 0.12)
else:
    m = ~((L > 0.45) & (hue < 128)) & ~((hue > 200) & (hue < 300) & (L > 0.6))
def band(lo, hi):
    q0, q1 = np.percentile(L[m], [lo, hi]); s = m & (L >= q0) & (L <= q1)
    a, b = lab[..., 1][s].mean(), lab[..., 2][s].mean()
    return f"L {L[s].mean():.3f} C {np.hypot(a, b):.3f} h {np.degrees(np.arctan2(b, a)) % 360:.0f}"
print(f"{label}: top 5% {band(95, 100)} | median band {band(47.5, 52.5)}")
