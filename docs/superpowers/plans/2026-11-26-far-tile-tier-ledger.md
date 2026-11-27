# Far tile tier — ledger

Plan: `2026-11-26-far-tile-tier.md`. Every number: command, raw output
line, commit, `Render WxH`. Artifacts in `.superpowers/far/` (worktree).

Capture helper (`.superpowers/far/cap.sh BIN VIEW OUT.png [flags]`) runs
`flock /tmp/meridian-gpu.lock BIN $SCENE --camera $VIEW --capture OUT.png
[flags]` with `SCENE="--terrain assets/terrain/relief-0.5.height --trees
--tree-palette chlorophyll1 --scenery broadleaf --haze clean --sun 0.707
0.700 -0.707 --grade .superpowers/far/grade.json"` (grade =
`../meridian-scene/.superpowers/scene/r2b/grade-ev-1.25.json`, the
handover's), views A1 `1300 1350 1300 -0.785 -0.25`, H2 `850 1023.2 750
-0.785 0`, H1 `-875 1435 -900 3.1416 -0.12`.

## Setup

- `scene-broadleaf` (13469bf) merged into `far-tier` before stage 0
  (ffc505b), so every capture carries the current look flags.
- DLSS SDK from `../meridian/build-release/_deps/dlss_sdk-src` (the
  `../meridian-scene/build-release` path in the brief does not exist).
- Baseline binary: worktree `.superpowers/far/base` at ffc505b (the merge,
  before any change), `base/build-release`.

## Stage 0

### 0.1 premise read (cell_cull.comp, farfield.glsl, voxel.frag/mesh, renderer_farfield.cpp)

Matches the spec: per 64 m placement cell, brick columns at 4/8/16 m,
splatted once at load from the placed table, grid = placement grid,
one palette entry per (prototype, mesh slot), `s.occlusion = 1`, level =
coarsest whose voxel is <= 1 px.

False in the spec's "Meridian today":
- "takes over only where a cell's tallest tree is under 6 px (about 5 km)":
  with the broadleaf scenery the tallest tree per cell is 36.7/40.9/41.8 m
  (p10/p50/p90; temporary print, h1-default.log line 24), so 6 px is
  41 m x 1828 px/km / 6 = 12.5 km at 2560x1440, 70 deg horizontal fov
  (pixels per meter = 1828 / d). Nothing in the 4 km crop is ever farther
  than ~5.7 km, so the far field draws nothing at the scene views:
  `far cells 0` at a1 and H1 with the default.
- Consequence for stage 0's N: "4 m level at 1 px" is 7.3 km (N = 10 for
  41 m trees), also beyond every crop cell, so it equals the default. Runs
  taken instead: N = 20 (4 m voxel = 2 px, switch at 3.75 km) and N = 41
  (switch at 1.83 km, where a 1 m voxel is 1 px: stage 2's target).
- Run-to-run noise: two captures of the same view differ by ~0.5/255 mean
  over half the pixels, sky included (h1-default vs h1-n20, which differ
  only in 44 far cells: 0.48; a1-default vs a1-n20: 0.55). CLAUDE.md's
  "~1e-6/255" does not hold with these scene flags. 0.2's check is
  therefore against that floor.

### 0.2 `--far-switch-px N` — fddb121

- a1 base (ffc505b) vs a1 default (fddb121): mean max-channel difference
  0.53/255, 50.6% of pixels differ, max 17 — the same as the noise floor
  above (0.48-0.55), so no change beyond noise.

### 0.3 captures (2560x1440; every log has `Render 2560x1440, display 2560x1440, ... DLSS`)

Commands: `.superpowers/far/cap.sh ./build-release/game/game A1|H1
.superpowers/far/s0/<view>-<run>.png [--far-switch-px N]`. Default runs
at fddb121; the N runs at fddb121 plus the then-uncommitted, inert (mirror
off) placement fold of task 1.1 in the binary.

| view, run | far cells | voxel trees ms | far field ms | voxel quads | cull clusters ms | GPU ms |
|---|---|---|---|---|---|---|
| a1 default | 0 | 6.64 | 0.00 | 16164892 | 4.15 | 21.90 |
| a1 N=20 | 294 | 6.19 | 0.47 | 16107985 | 4.10 | 21.80 |
| a1 N=41 | 1768 | 5.00 | 2.71 | 12541958 | 2.80 | 21.20 |
| H1 default | 0 | 6.29 | 0.00 | 18107873 | 4.54 | 22.31 |
| H1 N=20 | 44 | 6.30 | 0.04 | 18096890 | 4.38 | 22.33 |
| H1 N=41 | 884 | 5.48 | 1.28 | 15273910 | 3.46 | 21.55 |

Raw lines (a1-n41.log):
`Rendered 120 frames, GPU 21.20 ms, 15520 drawn instances, ... far cells 1768, voxel trees 165482 (bricks 2252530, voxels 12541958), ... 0 dropped by full cull lists ... 0 validation errors`;
`GPU passes: ... cull clusters 2.80, ... voxel trees 5.00, far field 2.71, ...`.
No cull list overflow in any run, cascades included (`0 dropped` on every
`cull list need` line).

Look: `.superpowers/far/s0/crop-a1.png` (1:1 switch band, default / N=20 /
N=41, beside a 1:1 far-forest crop of 7.jpg): at 1.8 km the far cells are
visible 4 m blocks (~4 px), lighter and greyer than the cluster voxels
(one palette colour per mesh slot, no occlusion), with a hard switch
line. The present tier cannot take over at 1.8 km without stage 3.

Saving: at a1, N=41 takes 1.64 ms off the voxel pass and 1.35 ms off
cluster culling, but the far march costs 2.71 ms: net 0.7 ms GPU.
