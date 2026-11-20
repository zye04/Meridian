#!/usr/bin/env python3
"""Frame-to-frame flicker of a capture sequence over a crop. Usage:
  cloud-flicker.py [--crop X0,Y0,X1,Y1] FRAME.png FRAME.png ...
Crop in fractions of the frame (default the sky, 0,0,1,0.45). Per
consecutive pair: the signed luma difference /255, 4x4 box-blurred (pixel
noise the eye integrates is kept, single-pixel AA residue is not), then
its magnitude, pooled over all pairs: prints `mean`, `p99`, `p99.9`.
For a static camera (a turn is not a global translation of the frame)."""
import subprocess
import sys

import numpy as np


def luma(path):
    size = subprocess.run(["magick", path, "-format", "%w %h", "info:"], capture_output=True,
                          check=True).stdout.decode().split()
    w, h = int(size[0]), int(size[1])
    raw = subprocess.run(["magick", path, "-depth", "8", "rgb:-"], capture_output=True,
                         check=True).stdout
    rgb = np.frombuffer(raw, np.uint8).reshape(h, w, 3).astype(np.float64)
    return rgb @ np.array([0.2126, 0.7152, 0.0722])


def box4(a):
    h, w = (a.shape[0] // 4) * 4, (a.shape[1] // 4) * 4
    return a[:h, :w].reshape(h // 4, 4, w // 4, 4).mean(axis=(1, 3))


def main(argv):
    crop = (0.0, 0.0, 1.0, 0.45)
    frames = []
    i = 0
    while i < len(argv):
        if argv[i] == "--crop":
            i += 1
            crop = tuple(float(v) for v in argv[i].split(","))
        else:
            frames.append(argv[i])
        i += 1
    if len(frames) < 2:
        print(__doc__)
        return 2
    diffs = []
    previous = None
    for path in frames:
        image = luma(path)
        h, w = image.shape
        x0, y0, x1, y1 = (int(crop[0] * w), int(crop[1] * h), int(crop[2] * w), int(crop[3] * h))
        current = image[y0:y1, x0:x1]
        if previous is not None:
            diffs.append(np.abs(box4(current - previous)).ravel())
        previous = current
    pooled = np.concatenate(diffs)
    print(f"mean {pooled.mean():.4f}")
    print(f"p99 {np.percentile(pooled, 99):.4f}")
    print(f"p99.9 {np.percentile(pooled, 99.9):.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
