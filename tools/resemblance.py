#!/usr/bin/env python3
"""OKLCh region statistics of a Meridian capture against a reference frame.

usage: python3 -I tools/resemblance.py OURS.png REF.png --regions regions.json --view NAME

regions.json: {view: {"ours": {region: [x0,y0,x1,y1]}, "ref": {...}}} in
normalized [0,1] coordinates of each image (the ref without its black side
bars, which are cropped here). A region present on only one side is skipped.
The reference is resized to our height before the whole-frame rows
(contrast, sharpness) so both see the same pixel scale. Deltas are ours - ref.
"""
import argparse, json, subprocess, sys
import numpy as np

REGIONS = ["sky", "foliage-lit", "foliage-shadow", "grass", "far"]


def load(path):
    p = subprocess.run(["magick", path, "-alpha", "off", "-depth", "8", "rgb:-"], capture_output=True, check=True)
    w, h = map(int, subprocess.run(["magick", "identify", "-format", "%w %h", path + "[0]"], capture_output=True, check=True).stdout.split())
    return np.frombuffer(p.stdout, np.uint8).reshape(h, w, 3)


def crop_bars(a):
    cols = np.where((a.max(axis=2) > 8).mean(axis=0) > 0.5)[0]
    return a[:, cols[0]:cols[-1] + 1]


def resize_to_height(a, h):
    if a.shape[0] == h:
        return a
    w = round(a.shape[1] * h / a.shape[0])
    p = subprocess.run(["magick", "-size", f"{a.shape[1]}x{a.shape[0]}", "-depth", "8", "rgb:-", "-filter", "Lanczos",
                        "-resize", f"{w}x{h}!", "rgb:-"], input=a.tobytes(), capture_output=True, check=True)
    return np.frombuffer(p.stdout, np.uint8).reshape(h, w, 3)


def oklab(a):
    c = a.astype(np.float64) / 255
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    r, g, b = lin[..., 0], lin[..., 1], lin[..., 2]
    l = np.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    m = np.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    s = np.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    return (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)


def stats(a):
    L, A, B = oklab(a)
    C = np.hypot(A, B)
    h = np.degrees(np.arctan2(B.mean(), A.mean())) % 360  # circular mean (chroma-weighted)
    return {"L": L.mean(), "C": C.mean(), "h": h, "L p5": np.percentile(L, 5), "L p95": np.percentile(L, 95)}


def laplacian_std(L):
    k = 4 * L[1:-1, 1:-1] - L[:-2, 1:-1] - L[2:, 1:-1] - L[1:-1, :-2] - L[1:-1, 2:]
    return k.std()


def rect(a, r):
    h, w = a.shape[:2]
    x0, y0, x1, y1 = r
    return a[int(y0 * h):max(int(y1 * h), int(y0 * h) + 1), int(x0 * w):max(int(x1 * w), int(x0 * w) + 1)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours"); ap.add_argument("ref")
    ap.add_argument("--regions", required=True); ap.add_argument("--view", required=True)
    n = ap.parse_args()
    spec = json.load(open(n.regions))[n.view]
    ours = crop_bars(load(n.ours)); ref = crop_bars(load(n.ref))
    rows = []

    def row(name, o, r, hue=False):
        d = o - r
        if hue:
            d = (d + 180) % 360 - 180
        rows.append((name, o, r, d))

    sts = {}
    for reg in REGIONS:
        if reg in spec["ours"] and reg in spec["ref"]:
            so, sr = stats(rect(ours, spec["ours"][reg])), stats(rect(ref, spec["ref"][reg]))
            sts[reg] = (so, sr)
            for k in so:
                row(f"{reg} {k}", so[k], sr[k], k == "h")
    if "foliage-lit" in sts and "foliage-shadow" in sts:
        row("shadow/sunlit", sts["foliage-shadow"][0]["L"] / sts["foliage-lit"][0]["L"],
            sts["foliage-shadow"][1]["L"] / sts["foliage-lit"][1]["L"])
    refs = resize_to_height(ref, ours.shape[0])
    Lo, Lr = oklab(ours)[0], oklab(refs)[0]
    row("contrast (std L)", Lo.std(), Lr.std())
    row("sharpness (lap std L)", laplacian_std(Lo), laplacian_std(Lr))
    print(f"{'metric':<26}{'ours':>9}{'ref':>9}{'delta':>9}")
    for name, o, r, d in rows:
        print(f"{name:<26}{o:9.3f}{r:9.3f}{d:9.3f}")


main()
