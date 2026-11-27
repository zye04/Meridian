#!/usr/bin/env python3
"""Leaf sparkle count (decisions.md "Foliage specular AA"): pixels whose
OKLab lightness L exceeds 0.85, per 1e4 pixels of the image or rect.
Usage: white-dots.py A.png [X0 Y0 X1 Y1]   (top-left origin, end exclusive)"""
import sys

import numpy as np
from PIL import Image


def oklab_lightness(srgb):
    c = srgb / 255.0
    linear = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    l = 0.4122214708 * linear[..., 0] + 0.5363325363 * linear[..., 1] + 0.0514459929 * linear[..., 2]
    m = 0.2119034982 * linear[..., 0] + 0.6806995451 * linear[..., 1] + 0.1073969566 * linear[..., 2]
    s = 0.0883024619 * linear[..., 0] + 0.2817188376 * linear[..., 1] + 0.6299787005 * linear[..., 2]
    return 0.2104542553 * np.cbrt(l) + 0.7936177850 * np.cbrt(m) - 0.0040720468 * np.cbrt(s)


def main():
    if len(sys.argv) not in (2, 6):
        sys.exit(__doc__)
    image = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.float64)
    if len(sys.argv) == 6:
        x0, y0, x1, y1 = (int(v) for v in sys.argv[2:])
        image = image[y0:y1, x0:x1]
    lightness = oklab_lightness(image)
    print(f"white-dots {(lightness > 0.85).sum() * 1e4 / lightness.size:.3f}")


if __name__ == "__main__":
    main()
