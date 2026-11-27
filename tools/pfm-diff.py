#!/usr/bin/env python3
"""Per-channel difference of two PFM images (e.g. --capture-guides buffers).
Usage: pfm-diff.py A.pfm B.pfm
Prints `mean r g b` and `max r g b` of |a - b|; exit 1 on a size mismatch or
a NaN/Inf in either image."""
import sys

import numpy as np


def read_pfm(path):
    with open(path, "rb") as f:
        kind = f.readline().strip()
        if kind not in (b"PF", b"Pf"):
            sys.exit(f"{path}: not a PFM")
        width, height = (int(v) for v in f.readline().split())
        scale = float(f.readline())
        channels = 3 if kind == b"PF" else 1
        data = np.fromfile(f, dtype="<f4" if scale < 0 else ">f4", count=width * height * channels)
    return data.reshape(height, width, channels)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = read_pfm(sys.argv[1]), read_pfm(sys.argv[2])
    if a.shape != b.shape:
        print(f"size mismatch {a.shape} {b.shape}")
        sys.exit(1)
    if not (np.isfinite(a).all() and np.isfinite(b).all()):
        print("NaN or Inf")
        sys.exit(1)
    d = np.abs(a.astype(np.float64) - b.astype(np.float64)).reshape(-1, a.shape[2])
    print("mean " + " ".join(f"{v:.6g}" for v in d.mean(0)))
    print("max " + " ".join(f"{v:.6g}" for v in d.max(0)))


if __name__ == "__main__":
    main()
