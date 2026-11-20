#!/usr/bin/env python3
"""Image gates for motion-clarity checks. Usage:
  image-gates.py laplacian A.png
  image-gates.py ratio MOTION.png STILL.png
  image-gates.py shimmer A.png B.png [NAME=X0,Y0,X1,Y1 ...]   (fractions)
  image-gates.py ssim A.png B.png
  image-gates.py diff A.png B.png
Prints `name value` lines."""
import re
import subprocess
import sys

import numpy as np

# Beech view at 1920x1080: crown 300x200+840+300, shadow 400x120+820+960.
DEFAULT_RECTS = {
    "crown": (840 / 1920, 300 / 1080, 1140 / 1920, 500 / 1080),
    "shadow": (820 / 1920, 960 / 1080, 1220 / 1920, 1080 / 1080),
}


def magick(*args):
    return subprocess.run(["magick", *args], capture_output=True, check=False)


def laplacian(path):
    r = magick(path, "-colorspace", "gray", "-morphology", "Convolve",
               "Laplacian:0", "-format", "%[fx:standard_deviation]\n", "info:")
    return float(r.stdout.decode().strip())


def pixels(path):
    size = magick(path, "-format", "%w %h", "info:").stdout.decode().split()
    w, h = int(size[0]), int(size[1])
    raw = magick(path, "-depth", "8", "rgb:-").stdout
    return np.frombuffer(raw, np.uint8).reshape(h, w, 3).astype(np.float64)


def ssim(a, b):
    r = magick("compare", "-metric", "SSIM", a, b, "null:")
    # IM7-derived: 1 - 2 * dissimilarity, slightly non-standard SSIM, so gates compare relatively.
    # IM 7 prints the dissimilarity (1 - SSIM) / 2, normalized in parentheses.
    m = re.search(r"\(([-+0-9.eE]+)\)", r.stderr.decode())
    if not m:
        sys.exit("ssim: no metric in output: " + r.stderr.decode())
    return 1.0 - 2.0 * float(m.group(1))


def parse_rects(specs):
    if not specs:
        return DEFAULT_RECTS
    rects = {}
    for s in specs:
        name, vals = s.split("=")
        rects[name] = tuple(float(v) for v in vals.split(","))
    return rects


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    cmd, args = argv[1], argv[2:]
    if cmd == "laplacian":
        print(f"laplacian {laplacian(args[0]):.4f}")
    elif cmd == "ratio":
        print(f"ratio {laplacian(args[0]) / laplacian(args[1]):.4f}")
    elif cmd == "ssim":
        print(f"ssim {ssim(args[0], args[1]):.4f}")
    elif cmd == "diff":
        a, b = pixels(args[0]), pixels(args[1])
        print(f"diff {np.abs(a - b).mean():.4f}")
    elif cmd == "shimmer":
        a, b = pixels(args[0]), pixels(args[1])
        h, w = a.shape[:2]
        for name, (x0, y0, x1, y1) in parse_rects(args[2:]).items():
            xs, xe = round(x0 * w), round(x1 * w)
            ys, ye = round(y0 * h), round(y1 * h)
            d = np.abs(a[ys:ye, xs:xe] - b[ys:ye, xs:xe]).mean()
            print(f"{name} {d:.4f}")
    else:
        sys.exit(__doc__)


main(sys.argv)
