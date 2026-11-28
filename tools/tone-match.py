#!/usr/bin/env python3
"""tone-match.py CAPDIR: for captures named <view>_<grade>.png (view o4, s1, h2, start, walk360), per image, display luma Y' (Rec.709 of sRGB, 0-255) percentiles p1/5/25/50/75/95/99 of
the whole frame, and mean OKLCh of three classes from tone-regions.json (the light lane's regions2.json): shaded forest = the
'shaded' rect (o4, s1) or the darker half of 'forest' (h2, ref5); lit crowns = the 'near' rect's top quarter by L
(o4, s1) or the brighter quarter of 'forest' (h2, ref5); meadow = the 'meadow' rect (not scored at s1: on our
frames that rect holds haze over the valley, not meadow). Grass is left out of forest
classes (L > 0.45, hue < 128), as pct.py. References: 1.jpg for o4 and s1, 5.jpg for h2 and start. Score: the
mean |dY'| over the seven percentiles / 10 plus, per class, 10 |dL| + 50 |dC| + |dh| / 30 (lower is nearer)."""
import json, os, sys, glob
import numpy as np
from PIL import Image
M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566], [0.0883024619, 0.2817188376, 0.6299787005]])
M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099], [0.0259040371, 0.7827717662, -0.8086757660]])
D = os.path.dirname(os.path.abspath(__file__)); R = json.load(open(f"{D}/tone-regions.json"))
REF = os.path.join(D, "..", "assets", "reference", "chlorophyll", "twitter-2026-11-25")
P = [1, 5, 25, 50, 75, 95, 99]
def load(f):
    c = np.asarray(Image.open(f).convert("RGB"), np.float64) / 255
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    return 255 * (c @ np.array([0.2126, 0.7152, 0.0722])), np.cbrt(lin @ M1.T) @ M2.T
def lch(lab, m):
    a, b = lab[..., 1][m].mean(), lab[..., 2][m].mean()
    return (lab[..., 0][m].mean(), np.hypot(a, b), np.degrees(np.arctan2(b, a)) % 360)
def classes(lab, regions):
    L = lab[..., 0]; hue = np.degrees(np.arctan2(lab[..., 2], lab[..., 1])) % 360
    grass = (L > 0.45) & (hue < 128)
    def rect(name):
        x, y, w, h = regions[name]; m = np.zeros(L.shape, bool); m[y:y + h, x:x + w] = True; return m
    out = {}
    if "shaded" in regions:
        out["shaded"] = lch(lab, rect("shaded") & ~grass)
        near = rect("near") & ~grass; q = np.percentile(L[near], 75); out["lit"] = lch(lab, near & (L >= q))
    else:
        f = rect("forest") & ~grass; q50, q75 = np.percentile(L[f], [50, 75])
        out["shaded"] = lch(lab, f & (L <= q50)); out["lit"] = lch(lab, f & (L >= q75))
    if "meadow" in regions:
        out["meadow"] = lch(lab, rect("meadow"))
    return out
def stats(f, regions):
    y, lab = load(f)
    return np.percentile(y, P), (classes(lab, regions) if regions else {})
refs = {"o4": (f"{REF}/1.jpg", "ref1", "o4"), "s1": (f"{REF}/1.jpg", "ref1", "s1"),
        "h2": (f"{REF}/5.jpg", "ref5", "h2"), "start": (f"{REF}/5.jpg", "ref5", None),
        "walk360": (None, None, None)}
def fmt(p, c):
    s = "Y' " + " ".join(f"{v:5.1f}" for v in p)
    for k, (l, ch, h) in c.items(): s += f" | {k} {l:.3f} {ch:.3f} {h:3.0f}"
    return s
cap = sys.argv[1]; scores = {}
refstats = {}
for view, (rf, rk, ok) in refs.items():
    if rf: refstats[view] = stats(rf, R[rk])
for view in refs:
    files = sorted(glob.glob(f"{cap}/{view}_*.png"))
    if not files: continue
    rf, rk, ok = refs[view]
    if rf: print(f"{view:8s} {'ref':8s} {fmt(*refstats[view])}")
    for f in files:
        g = os.path.basename(f)[len(view) + 1:-4]
        p, c = stats(f, R[ok] if ok else None)
        line = f"{view:8s} {g:8s} {fmt(p, c)}"
        if rf:
            rp, rc = refstats[view]
            sc = np.abs(p - rp).mean() / 10 + sum(10 * abs(c[k][0] - rc[k][0]) + 50 * abs(c[k][1] - rc[k][1]) +
                     min(abs(c[k][2] - rc[k][2]), 360 - abs(c[k][2] - rc[k][2])) / 30 for k in c if k in rc and not (view == "s1" and k == "meadow"))
            scores.setdefault(g, {})[view] = sc; line += f" | score {sc:.2f}"
        print(line)
print("score per grade (lower nearer; views):")
for g, s in sorted(scores.items(), key=lambda kv: sum(kv[1].values())):
    print(f"  {g:8s} total {sum(s.values()):6.2f}  " + "  ".join(f"{v} {x:.2f}" for v, x in s.items()))
