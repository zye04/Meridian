"""Forest truth for --score-forest: canopy height (surface minus ground,
both float32 m, row 0 north, SIZE x SIZE) per cell of CELL x CELL samples;
a cell is forest when canopy over 3 m covers at least 20% of it (close to
the Swiss NFI forest definition). Writes u32 width, height, cell size in
samples, then one byte per cell (1 forest), row 0 north.

usage: canopy-truth.py ground.f32 surface.f32 SIZE CELL out.bin
"""
import struct
import sys

import numpy as np

ground_path, surface_path, size, cell, out_path = sys.argv[1:6]
size, cell = int(size), int(cell)
ground = np.fromfile(ground_path, dtype="<f4").reshape(size, size)
surface = np.fromfile(surface_path, dtype="<f4").reshape(size, size)
tall = (surface - ground) > 3.0
cells = size // cell
cover = tall[: cells * cell, : cells * cell].reshape(cells, cell, cells, cell).mean(axis=(1, 3))
forest = (cover >= 0.2).astype(np.uint8)
with open(out_path, "wb") as f:
    f.write(struct.pack("<III", cells, cells, cell))
    f.write(forest.tobytes())
print(f"Wrote {out_path} ({cells}x{cells} cells of {cell} samples), forest {100 * forest.mean():.1f}%")
