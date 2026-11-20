#!/usr/bin/env python3
"""Cloud look metrics. Usage:
  cloud-metrics.py capture ON.pfm EV_ON OFF.pfm EV_OFF   (linear HDR, clouds on/off)
  cloud-metrics.py photo IMAGE                           (sRGB image, color-segmented)
Prints `name value` lines:
  top    sunlit tops (95th percentile cloud luminance) / adjacent clear sky
  mid    median cloud luminance / the tops' (shading contrast inside clouds)
  base   cloud bases (bottom edges) / the clear sky just below them
  spread vertical spread of each cloud's bottom edge (10-90th percentile of
         its rows) / the cloud's height, median over clouds (0 = flat base)
  top_cd, sky_cd (captures) the tops' luminance and the median clear sky
         behind the clouds, cd/m^2
  cover  fraction of the sky covered by clouds
  clouds number of clouds measured (connected areas over 0.05% of the frame)
A capture's adjacent sky is the clouds-off luminance at the same pixel, with
both images' pre-exposure divided out (EV100 from the run's log); a photo's
is the clear sky 6-12 px below. Captures only measure the sky (blue over red
in the clouds-off image)."""
import subprocess
import sys

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722])


def load_pfm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"PF"
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, "<f4" if scale < 0 else ">f4")
    return data.reshape(height, width, 3)[::-1].astype(np.float64)


def load_srgb(path):
    size = subprocess.run(["magick", path, "-format", "%w %h", "info:"], capture_output=True,
                          check=True).stdout.split()
    w, h = int(size[0]), int(size[1])
    raw = subprocess.run(["magick", path, "-depth", "8", "rgb:-"], capture_output=True,
                         check=True).stdout
    c = np.frombuffer(raw, np.uint8).reshape(h, w, 3) / 255.0
    return c, np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def label(mask):
    """Connected components (4-neighbour) from vertical runs, union-find."""
    h, w = mask.shape
    parent = []

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    labels = np.zeros(mask.shape, np.int32)
    previous = []
    for x in range(w):
        column = np.concatenate(([0], mask[:, x].astype(np.int8), [0]))
        edges = np.flatnonzero(np.diff(column))
        runs = []
        for y0, y1 in zip(edges[::2], edges[1::2]):
            r = len(parent)
            parent.append(r)
            for p0, p1, q in previous:
                if p0 < y1 and y0 < p1:
                    a, b = find(r), find(q)
                    parent[max(a, b)] = min(a, b)
            runs.append((y0, y1, r))
        for y0, y1, r in runs:
            labels[y0:y1, x] = r + 1
        previous = runs
    roots = np.array([find(i) for i in range(len(parent))], np.int32)
    return np.concatenate(([0], roots + 1))[labels]


def measure(cloud, ratio_top, base_ratio_at, lum):
    h, w = cloud.shape
    labels = label(cloud)
    ids, counts = np.unique(labels[labels > 0], return_counts=True)
    spreads = []
    bases = []
    for i in ids[counts > 0.0005 * h * w]:
        ys, xs = np.nonzero(labels == i)
        height = ys.max() - ys.min() + 1
        bottom = np.full(w, -1)
        np.maximum.at(bottom, xs, ys)
        columns = np.flatnonzero(bottom >= 0)
        rows = bottom[columns]
        spreads.append((np.percentile(rows, 90) - np.percentile(rows, 10)) / height)
        for x, y in zip(columns, rows):
            v = base_ratio_at(x, y)
            if v is not None:
                bases.append(v)
    print(f"top {ratio_top:.3f}")
    print(f"mid {np.median(lum[cloud]) / np.percentile(lum[cloud], 95):.3f}")
    print(f"base {np.median(bases) if bases else float('nan'):.3f}")
    print(f"spread {np.median(spreads) if spreads else float('nan'):.3f}")
    print(f"clouds {len(spreads)}")


def capture(on_path, ev_on, off_path, ev_off):
    # Pre-exposure is 1 / (1.2 2^EV100) (exposure.comp): cd/m^2.
    on = load_pfm(on_path) @ LUMA * 1.2 * 2.0 ** float(ev_on)
    off_rgb = load_pfm(off_path)
    off = off_rgb @ LUMA * 1.2 * 2.0 ** float(ev_off)
    sky = off_rgb[..., 2] > off_rgb[..., 0]
    ratio = on / np.maximum(off, 1e-9)
    cloud = sky & (np.abs(ratio - 1.0) > 0.05)
    h = cloud.shape[0]
    if cloud.any():
        print(f"top_cd {np.percentile(on[cloud], 95):.0f}")
        print(f"sky_cd {np.median(off[cloud]):.0f}")
    print(f"cover {cloud.sum() / max(sky.sum(), 1):.3f}")

    def base_at(x, y):
        y = y - 3
        return ratio[y, x] if y >= 0 and cloud[y, x] and y + 4 < h else None

    measure(cloud, np.percentile(ratio[cloud], 95) if cloud.any() else float("nan"), base_at, on)


def photo(path):
    srgb, linear = load_srgb(path)
    lum = linear @ LUMA
    hi, lo = srgb.max(axis=2), srgb.min(axis=2)
    saturation = (hi - lo) / np.maximum(hi, 1e-6)
    cloud = (saturation < 0.3) & (hi > 0.45)
    sky = (saturation >= 0.3) & (srgb[..., 2] > srgb[..., 0] + 0.1)
    h = cloud.shape[0]
    sky_level = np.median(lum[sky])
    print(f"cover {cloud.sum() / max((cloud | sky).sum(), 1):.3f}")

    def base_at(x, y):
        if y < 3 or y + 12 >= h:
            return None
        below = sky[y + 6:y + 12, x]
        return lum[y - 3, x] / lum[y + 6:y + 12, x][below].mean() if below.all() else None

    measure(cloud, np.percentile(lum[cloud], 95) / sky_level, base_at, lum)


if __name__ == "__main__":
    if len(sys.argv) == 6 and sys.argv[1] == "capture":
        capture(*sys.argv[2:])
    elif len(sys.argv) == 3 and sys.argv[1] == "photo":
        photo(sys.argv[2])
    else:
        sys.exit(__doc__)
