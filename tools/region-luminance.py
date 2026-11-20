#!/usr/bin/env python3
"""Mean Rec.709 luminance of pixel rects in a .pfm capture, and each one's
ratio to the first. Rects are X0 Y0 X1 Y1: top-left origin, exclusive end."""
import sys

import numpy as np


def load_pfm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"PF"
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, "<f4" if scale < 0 else ">f4")
    return data.reshape(height, width, 3)[::-1]


if len(sys.argv) < 6 or (len(sys.argv) - 2) % 4:
    sys.exit("usage: region-luminance.py in.pfm X0 Y0 X1 Y1 [X0 Y0 X1 Y1 ...]")
image = load_pfm(sys.argv[1])
luminance = image @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
coords = list(map(int, sys.argv[2:]))
means = []
for x0, y0, x1, y1 in zip(*[iter(coords)] * 4):
    means.append(float(luminance[y0:y1, x0:x1].mean()))
    print(f"{x0} {y0} {x1} {y1}: {means[-1]:.6g}  ratio {means[-1] / means[0]:.4f}")
