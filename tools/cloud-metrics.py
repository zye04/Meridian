#!/usr/bin/env python3
"""Cloud look metrics. Usage:
  cloud-metrics.py capture ON.pfm EV_ON OFF.pfm EV_OFF   (linear HDR, clouds on/off)
  cloud-metrics.py photo IMAGE                           (sRGB image, color-segmented)
Prints `name value` lines:
  top    sunlit tops: 95th percentile of each cloud pixel's luminance over
         its adjacent clear sky
  mid    median cloud luminance / 95th percentile (shading contrast)
  base   cloud bases (1% of the image height inside each cloud's bottom
         edge) / the adjacent clear sky; base_columns: columns measured /
         bottom-edge columns seen (the rest fail the sky test)
  spread vertical spread of each cloud's bottom edge (10-90th percentile of
         its rows) / the cloud's height, median over clouds (0 = flat base)
  top_cd, sky_cd (captures) the tops' luminance and the median clear sky
         behind the clouds, cd/m^2
  cover  fraction of the sky covered by clouds
  clouds number of clouds measured (connected areas over 0.05% of the frame)
A capture's adjacent sky is the clouds-off luminance at the same pixel, with
both images' pre-exposure divided out (EV100 from the run's log); captures
only measure the sky (blue over red in the clouds-off image), so they
are not meaningful for views whose sky or ground is red (sunset). A photo's
clouds are achromatic pixels (saturation < 0.3, value > 0.15), its sky
saturated blue; adjacent sky is the median sky within 4% of the image
height around each cloud (top), and for bases the sky 2-4% of the image
height below the edge where at least half of it is sky (a photo is
display-referred; clouds_without_sky counts clouds with no sky near them)."""
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


def measure(cloud, labels, ratio, lum, base_at):
    """ratio: each cloud pixel's luminance over its adjacent clear sky."""
    h, w = cloud.shape
    ids, counts = np.unique(labels[labels > 0], return_counts=True)
    inset = max(1, round(0.01 * h))
    spreads = []
    bases = []
    columns_seen = 0
    for i in ids[counts > 0.0005 * h * w]:
        ys, xs = np.nonzero(labels == i)
        height = ys.max() - ys.min() + 1
        bottom = np.full(w, -1)
        np.maximum.at(bottom, xs, ys)
        columns = np.flatnonzero(bottom >= 0)
        rows = bottom[columns]
        spreads.append((np.percentile(rows, 90) - np.percentile(rows, 10)) / height)
        for x, y in zip(columns, rows):
            columns_seen += 1
            if y - inset >= 0 and cloud[y - inset, x]:
                v = base_at(x, y, inset)
                if v is not None:
                    bases.append(v)
    print(f"top {np.percentile(ratio[cloud], 95):.3f}")
    print(f"mid {np.median(lum[cloud]) / np.percentile(lum[cloud], 95):.3f}")
    print(f"base {np.median(bases) if bases else float('nan'):.3f}")
    print(f"base_columns {len(bases)}/{columns_seen}")
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
    if not cloud.any():
        sys.exit("no clouds")
    print(f"top_cd {np.percentile(on[cloud], 95):.0f}")
    print(f"sky_cd {np.median(off[cloud]):.0f}")
    print(f"cover {cloud.sum() / max(sky.sum(), 1):.3f}")
    measure(cloud, label(cloud), ratio, on, lambda x, y, inset: ratio[y - inset, x])


def photo(path):
    srgb, linear = load_srgb(path)
    lum = linear @ LUMA
    hi, lo = srgb.max(axis=2), srgb.min(axis=2)
    saturation = (hi - lo) / np.maximum(hi, 1e-6)
    cloud = (saturation < 0.3) & (hi > 0.15)
    sky = (saturation >= 0.3) & (srgb[..., 2] > srgb[..., 0] + 0.1)
    h, w = cloud.shape
    print(f"cover {cloud.sum() / max((cloud | sky).sum(), 1):.3f}")
    labels = label(cloud)
    # Adjacent sky: the median clear sky within 4% of the image height
    # around each cloud.
    ratio = np.zeros_like(lum)
    margin = max(2, round(0.04 * h))
    rejected = 0
    for i in np.unique(labels[labels > 0]):
        ys, xs = np.nonzero(labels == i)
        y0, y1 = max(ys.min() - margin, 0), min(ys.max() + margin + 1, h)
        x0, x1 = max(xs.min() - margin, 0), min(xs.max() + margin + 1, w)
        ring = lum[y0:y1, x0:x1][sky[y0:y1, x0:x1]]
        if ring.size == 0:
            rejected += 1
            cloud[ys, xs] = False
            continue
        ratio[ys, xs] = lum[ys, xs] / np.median(ring)
    print(f"clouds_without_sky {rejected}")

    def base_at(x, y, inset):
        window = slice(y + 2 * inset, y + 4 * inset + 1)
        below = sky[window, x]
        if y + 4 * inset >= h or below.mean() < 0.5:
            return None
        return lum[y - inset, x] / lum[window, x][below].mean()

    measure(cloud, labels * cloud, ratio, lum, base_at)


if __name__ == "__main__":
    if len(sys.argv) == 6 and sys.argv[1] == "capture":
        capture(*sys.argv[2:])
    elif len(sys.argv) == 3 and sys.argv[1] == "photo":
        photo(sys.argv[2])
    else:
        sys.exit(__doc__)
