#!/usr/bin/env python3
"""Per-segment fps, 1% lows and pass ledger of a --route benchmark.

usage: python3 -I tools/bench-route.py BENCH.csv FILE.route
"""
import math
import sys

import numpy as np


def read_segments(path):
    keys = []
    for line in open(path):
        line = line.split("#")[0].split()
        if len(line) >= 6:
            keys.append((float(line[0]), line[6] if len(line) > 6 else None))
    t0, end = keys[0][0], keys[-1][0]
    starts = [(t - t0, name) for t, name in keys if name]
    bounds = [(name, s, starts[i + 1][0] if i + 1 < len(starts) else end - t0)
              for i, (s, name) in enumerate(starts)]
    return bounds, end - t0


def low1(ms):
    n = max(1, math.ceil(0.01 * len(ms)))
    return 1000.0 / np.sort(ms)[-n:].mean()


def row(name, ms, passes, cols):
    n = len(ms)
    means = sorted(((passes[c].mean(), c) for c in cols), reverse=True)[:4]
    ledger = ", ".join(f"{c} {m:.2f}" for m, c in means)
    return (f"{name:<16} {n:>6} {1000.0 / ms.mean():>8.1f} {low1(ms):>9.1f} "
            f"{ms.mean():>7.2f}  {ledger}")


def main():
    csv, route = sys.argv[1:3]
    data = np.genfromtxt(csv, delimiter=",", names=True)
    names = data.dtype.names
    cols = [c for c in names if c not in ("frame", "frame_ms", "cpu_ms", "gpu_ms")]
    frame_ms = data["frame_ms"]
    t = np.cumsum(frame_ms) / 1000.0
    bounds, duration = read_segments(route)
    print(f"{'segment':<16} {'frames':>6} {'avg_fps':>8} {'low1_fps':>9} {'avg_ms':>7}  top passes (ms)")
    for name, s, e in bounds:
        if t[-1] < s:
            print(f"{name:<16} not reached")
            continue
        sel = (t > s) & (t <= e)
        if not sel.any():
            print(f"{name:<16} not reached")
            continue
        line = row(name, frame_ms[sel], data[sel], cols)
        if 1000.0 / frame_ms[sel].mean() < 105:
            line += "  BELOW 105"
        print(line)
    sel = t <= duration
    line = row("route", frame_ms[sel], data[sel], cols)
    if 1000.0 / frame_ms[sel].mean() < 180:
        line += "  BELOW 180"
    print(line)


main()
