#!/usr/bin/env python3
"""A raster .pfm capture against a --reference one: per region, mean Rec.709
luminance of both, their ratio (reference / raster) and rgb normalized to G.

    reference-compare.py RASTER.pfm REFERENCE.pfm [--ev RASTER_EV REFERENCE_EV]
                         [--regions FILE --view NAME] [X0 Y0 X1 Y1 ...]

Both captures are pre-exposed with their run's exposure (the EV100 each run
prints); exposure goes as 2^-EV100, so the raster is scaled by
2^(RASTER_EV - REFERENCE_EV). Rects: top-left origin, exclusive end; none
given = a 4x4 grid. Exit code 1 if either image has a NaN or Inf pixel."""
import argparse
import json
import sys

import numpy as np

LUMINANCE = np.array([0.2126, 0.7152, 0.0722])


def load_pfm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"PF"
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, "<f4" if scale < 0 else ">f4")
    return data.reshape(height, width, 3)[::-1].astype(np.float64)


def chroma(rgb):
    return " ".join(f"{c / rgb[1]:.2f}" if rgb[1] > 0 else "-" for c in rgb)


parser = argparse.ArgumentParser()
parser.add_argument("raster")
parser.add_argument("reference")
parser.add_argument("--ev", nargs=2, type=float, default=[0.0, 0.0])
parser.add_argument("--regions")
parser.add_argument("--view")
parser.add_argument("rects", nargs="*", type=int)
args = parser.parse_args()

raster = load_pfm(args.raster) * 2.0 ** (args.ev[0] - args.ev[1])
reference = load_pfm(args.reference)
if raster.shape != reference.shape:
    sys.exit(f"sizes differ: {raster.shape[1]}x{raster.shape[0]} vs "
             f"{reference.shape[1]}x{reference.shape[0]}")
height, width, _ = raster.shape

regions = []
if args.regions:
    for name, region in json.load(open(args.regions))[args.view].items():
        regions.append((name, region["rect"]))
if len(args.rects) % 4:
    sys.exit("rects take 4 numbers each")
for i in range(0, len(args.rects), 4):
    regions.append((" ".join(map(str, args.rects[i:i + 4])), args.rects[i:i + 4]))
if not regions:
    for gy in range(4):
        for gx in range(4):
            rect = [gx * width // 4, gy * height // 4, (gx + 1) * width // 4,
                    (gy + 1) * height // 4]
            regions.append((f"cell {gx},{gy}", rect))

bad = int((~np.isfinite(raster)).any(axis=2).sum() + (~np.isfinite(reference)).any(axis=2).sum())
finite = np.isfinite(raster).all(axis=2) & np.isfinite(reference).all(axis=2)
for name, (x0, y0, x1, y1) in regions:
    mask = finite[y0:y1, x0:x1]
    a = raster[y0:y1, x0:x1][mask].mean(axis=0)
    b = reference[y0:y1, x0:x1][mask].mean(axis=0)
    la, lb = a @ LUMINANCE, b @ LUMINANCE
    ratio = lb / la if la > 0 else float("nan")
    print(f"{name}: L raster {la:.4g} reference {lb:.4g} ratio {ratio:.3f}  "
          f"rgb/G raster {chroma(a)} reference {chroma(b)}")
la = raster[finite] @ LUMINANCE
lb = reference[finite] @ LUMINANCE
error = np.abs(la - lb).sum() / max(lb.sum(), 1e-12)
print(f"frame: rel. error {error:.4f}, NaN/Inf {bad}")
sys.exit(1 if bad else 0)
