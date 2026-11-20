#!/usr/bin/env python3
"""Exposure-normalized radiance ratio of two .pfm captures of the same view,
with and without placement (--placement off), per pixel rect.

Each rect's per-channel mean is divided by the sky rect's mean luminance in
the same capture (the sky is unaffected by placement, so this cancels the
auto-exposure), then the 'with' value is divided by the 'without' one. 1.0
means the terrain alone is as bright as the terrain under the meadow.
Rects are X0 Y0 X1 Y1: top-left origin, exclusive end; the first rect is the
sky. Usage: placement-match.py with.pfm without.pfm SKY X0 Y0 X1 Y1 [rect ...]
Rows: R G B luminance ratios."""
import sys

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)


def load_pfm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"PF"
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, "<f4" if scale < 0 else ">f4")
    return data.reshape(height, width, 3)[::-1].astype(np.float64)


def region_means(image, rect):
    x0, y0, x1, y1 = rect
    return image[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)


def main(argv):
    if len(argv) < 11 or (len(argv) - 3) % 4:
        sys.exit(__doc__)
    with_img, without_img = load_pfm(argv[1]), load_pfm(argv[2])
    coords = list(map(int, argv[3:]))
    rects = list(zip(*[iter(coords)] * 4))
    sky_with = region_means(with_img, rects[0]) @ LUMA
    sky_without = region_means(without_img, rects[0]) @ LUMA
    print(f"sky luminance with {sky_with:.5g} without {sky_without:.5g} "
          f"(ratio {sky_with / sky_without:.4f})")
    for rect in rects[1:]:
        a = region_means(with_img, rect) / sky_with
        b = region_means(without_img, rect) / sky_without
        r = a / b
        print(f"{rect[0]} {rect[1]} {rect[2]} {rect[3]}: R {r[0]:.3f} G {r[1]:.3f} "
              f"B {r[2]:.3f} L {(a @ LUMA) / (b @ LUMA):.3f}")


main(sys.argv)
