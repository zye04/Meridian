#!/usr/bin/env python3
"""Albedo audit: the diffuse albedo the engine shaded with, per region.

Usage: albedo-audit.py ALBEDO.pfm REGIONS.json VIEW

ALBEDO.pfm comes from `game --capture X --capture-albedo ALBEDO.pfm`
(linear, render-sized). REGIONS.json maps view -> region -> {"rect": [x0,
y0, x1, y1] (top-left origin, end exclusive), "class": one of RANGES,
"source": optional texture path}. Per region it prints the mean linear
albedo and luminance, then:
  source: the source texture's mean (sRGB decoded, alpha-weighted), and the
          ratio rendered / source per channel: the engine-fidelity check
          (colour space, tint, factors, mips must not move it);
  range:  whether the luminance falls in the class's approximate physical
          range (informational: an outlier points at the asset or the
          cook's mapping, not the lighting).
"""
import json
import subprocess
import sys

import numpy as np

# Approximate visible-band luminance albedo (linear). Informational, not a
# gate: published charts (e.g. Lagarde's DONTNOD PBR chart) list broadband
# values, which run higher than visible reflectance for vegetation; leaf and
# bark follow Pilarski et al. 2008 PAR data (research/2026-11-23-gi-foliage-*).
RANGES = {
    "leaf": (0.03, 0.15),
    "grass": (0.05, 0.18),
    "bark": (0.05, 0.30),
    "soil": (0.05, 0.30),
    "litter": (0.05, 0.25),
    "rock": (0.10, 0.40),
}
LUMA = np.array([0.2126, 0.7152, 0.0722])


def read_pfm(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"PF":
            sys.exit(f"{path}: not an RGB PFM")
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, "<f4" if scale < 0 else ">f4")
    return data.reshape(h, w, 3)[::-1]


def source_mean(path):
    w, h = map(int, subprocess.check_output(
        ["magick", "identify", "-format", "%w %h", path + "[0]"]).split())
    raw = subprocess.check_output(["magick", path + "[0]", "-depth", "8", "rgba:-"])
    t = np.frombuffer(raw, np.uint8).reshape(h, w, 4).astype(np.float64) / 255
    rgb = np.where(t[..., :3] <= 0.04045, t[..., :3] / 12.92,
                   ((t[..., :3] + 0.055) / 1.055) ** 2.4)
    a = t[..., 3:]
    return (rgb * a).sum((0, 1)) / max(a.sum(), 1e-9)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    albedo = read_pfm(sys.argv[1])
    regions = json.load(open(sys.argv[2]))[sys.argv[3]]
    for name, r in regions.items():
        x0, y0, x1, y1 = r["rect"]
        px = albedo[y0:y1, x0:x1].reshape(-1, 3)
        # The sky writes exactly 0.5 into the albedo guide.
        px = px[np.abs(px - 0.5).max(1) > 1e-3]
        m = px.mean(0)
        y = m @ LUMA
        lo, hi = RANGES[r["class"]]
        verdict = "in" if lo <= y <= hi else ("LOW" if y < lo else "HIGH")
        line = (f"{name:14} {r['class']:6} rgb {m[0]:.3f} {m[1]:.3f} {m[2]:.3f}"
                f"  Y {y:.3f}  range {lo:.2f}-{hi:.2f} {verdict}")
        if "source" in r:
            s = source_mean(r["source"])
            ratio = m / np.maximum(s, 1e-6)
            line += (f"  source {s[0]:.3f} {s[1]:.3f} {s[2]:.3f}"
                     f"  ratio {ratio[0]:.2f} {ratio[1]:.2f} {ratio[2]:.2f}")
        print(line)


main()
