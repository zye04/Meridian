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

## Stage 1 — 6bd5aed

Debug (`./build/game/game`, a1, `--frames 4`, `.superpowers/far/s1/a1-debug.log`):
- `Far field grid: 512x512 cells from cell (-256, -256), L0/L1 bands 128x128/252x252 cells; transient placement 23372110 instances in 19 ms (chunks of 101 rows)`
- `far field check: 13 cells, 35072 voxels (18650 occupied) compared with the CPU replica, 0 mismatches`
- `far placement check: 6318 points (3575 kept) in the sample windows compared with the CPU reference, 0 mismatches`
- `far shadow map check: 8192 texels (3996 with a hit) ... max difference 0.383 m (0 over 1 m), 0 hit/empty mismatches, 0 texels out of steps`
- `far field ray check: 256 rays (53 partly transmitted) ... max difference 8.7e-07`
- `Rendered 4 frames, ... far cells 60050, ... 0 dropped by full cull lists ..., 0 validation errors`
- Crop bricks against main: the whole placement grid's hash differs
  (0x2761... vs base 0x2f14...): crowns of mirrored trees just past the
  crop reach the border cells (expected). Interior (outer ring + 1 cells
  excluded), Debug a1 `--frames 2`:
  - 6bd5aed + hash: `far field hash 0x772f31c1e61717ba, placement grid inside its outer ring+1 cells 0xd57ea4627115d02a`
  - base ffc505b + hash: `far field hash 0x749bf067eb98cd52, placement grid inside its outer ring+1 cells 0x35d39cce445d2399`
  - Cause, from a per-column dump of both (`.superpowers/far/s1/dump-{far,base}.txt`:
    level, column, heightAndCount, content hash): 68491 of 75600 interior
    columns differ, only 14 in height/count; heightBase equal (817.991394).
    A global input: the splat took the trilinear weights relative to the
    far grid's corner (now ~24 km from crop positions instead of <= 15),
    so they round differently and fixed-point deposits move by a unit.
    Unit test (CPU splat, an 8x8 grid vs the same inside 150 cells more
    on each side): 17 voxels differ without the anchor, 0 with.
  - Fix: `FarFieldGrid::anchorX/Z`, positions relative to the placement
    grid's corner, the cell offset added as an integer (CPU, GPU, replica
    windows). Rerun (Debug a1 `--frames 2`, `.superpowers/far/s1/a1-debug4.log`):
    `far field hash 0x0897cc326a99f158, placement grid inside its outer ring+1 cells 0x35d39cce445d2399`
    = base. Replica checks 0 mismatches; `0 validation errors`.

Release (`.superpowers/far/cap.sh ./build-release/game/game A1|H2|H1 .superpowers/far/s1/<v>.png`, every log `Render 2560x1440, display 2560x1440 ... DLSS`, 0 dropped in all 8 cull list lines, cascades included):
- `Far field: bricks L0/L1/L2 1073100/731635/579831 ... resident 424 MiB (bricks 151/109/85, columns 32/8/2); peak transient 487 MiB, device memory allocated at peak 10306 MiB (all heaps 10403 MiB); templates 9 ms, splat 251 ms (L0/L1/L2 104/71/76), total 271 ms; 0 lost deposits`
- `Loaded: resident 9927 MiB` (stage 0: 9586; +341 MiB; gate 12288)
- `Far shadow map: box 32768 x 642 x 32768 m, up to 8388608 texels (16.0 MiB per map, 2 maps), 16 m texels at the default sun`

| view | far cells | far field ms | voxel trees ms | GPU ms | GPU ms stage 0 default |
|---|---|---|---|---|---|
| a1 | 60050 | 18.24 | 6.04 | 39.47 | 21.90 |
| H2 | 58012 | 5.94 | 6.83 | 31.55 | — |
| H1 | 49983 | 19.18 | 6.30 | 41.44 | 22.31 |

Look: `.superpowers/far/s1/sheet-stage1.png` (a1/H2/H1, the stage-0 a1
and H1 below, 5.jpg and 7.jpg as the reference row),
`.superpowers/far/s1/crop-h1.png` (1:1 horizon canopy at H1, before and
after, beside 7.jpg's far forest at 1:1). The bare mirrored hills are
forested to the horizon; the far canopy is stippled 4-16 m blocks,
bluer/greyer than 7.jpg (no shading work yet, as the director ruled).

Cost: the far pass is 18-19 ms at a1 and H1 (60k / 50k cells). Not yet
measured why (director's candidate: overlapping boxes marched empty per
pixel, no front-to-back early-out across cells): next step.

## Far pass cost (director's blocking task before stage 2)

All Release, `.superpowers/far/cap.sh <bin> A1|H1 .superpowers/far/cost/<v>-<tag>.png [flags]`,
every log `Render 2560x1440, display 2560x1440 ... DLSS`, 0 dropped, 0 validation errors;
the figure is `far field` in the run's `GPU passes:` line (120 frames).

| run | a1 far field ms | H1 far field ms | far cells a1 |
|---|---|---|---|
| stage 1 (6bd5aed) | 18.24 | 19.18 | 60050 |
| voxel.frag without the terrain shadow march (`noterrain`) | 5.87 (rerun 5.88) | 6.00 (rerun 6.00) | 60050 |
| march only, shading skipped (`noshade`) | 2.55 | 2.41 | 60050 |
| `--far-extent 4` | 4.17 | — | 3473 |
| `--far-extent 0` | 0.00 | — | 0 |
| far terrain window 2048^2 x 16 m (`farwin`) | 10.56 | 8.58 | 60050 |
| far terrain window 3072^2 x 16 m (`farwin3072`) | 6.49 | 6.58 | 60050 |
| + near-to-far draw in 8 distance bands (`bands`, reverted) | 6.39 | 6.60 | 60050 |

Cause: a far cell's lit fragments lie outside the 8 km terrain
shadow-height window and marched the height pyramid (`terrainOccludes`,
up to 20 km) per fragment: ~12.4 ms of 18.24. Not the director's
candidates: cost follows pixels (3473 cells at `--far-extent 4` still
4.17 ms), and front-to-back order gives nothing measurable.

Final (far window 3072 x 16 m, far shadow box limited to the L1 band), `.superpowers/far/cost/final-*.log`:
- a1: `... scene early 4.31, voxel trees 6.07, far field 6.43, ...`, `Rendered 120 frames, GPU 27.52 ms` (stage 1: 39.47)
- H2: `... scene early 7.29, voxel trees 6.77, far field 2.21, ...`, `GPU 27.95 ms` (stage 1: 31.55)
- H1: `... scene early 4.00, voxel trees 6.32, far field 6.55, ...`, `GPU 28.50 ms` (stage 1: 41.44)
- `Far shadow map: box 16128 x 641 x 16128 m, up to 8388608 texels (16.0 MiB per map, 2 maps), 8 m texels at the default sun (2835x1909)`; `first map 2853x1702 texels (8 m, 9.3 MiB used) built in 8 ms`
- `Loaded: resident 9927 MiB` (unchanged; the 72 MiB far window is allocated with the near map at the first frame)
- Debug a1 `--frames 2`: replica checks 0 mismatches (`far shadow map check: 8192 texels (6860 with a hit) ... max difference 0.248 m (0 over 1 m), 0 hit/empty mismatches`), `0 validation errors`.
- Look: whole-frame mean difference to stage 1 (tools/image-gates.py diff) a1 0.2659, H2 0.4640, H1 0.2265;
  noise (two runs, same look) a1 0.2559, H1 0.1980. Sheet `.superpowers/far/cost/sheet-farcost.png`.

## Stage 3: tile shading (director's order 2026-11-27: before stage 2)

All Release `build-release`, every log `Render 2560x1440, display 2560x1440 ... DLSS`,
0 dropped, 0 validation errors. Tile vs cluster voxels: `.superpowers/far/s3/views.sh
build-release/game/game <dir> --far-switch-px 41` (a1/h1 plain, lod, sun, base-color,
ambient in one launch; `measure.sh` for plain/lod/sun), table by `.superpowers/far/s3/canopy.py
premise/default <dir> a1|h1`: same-tree pixels = far-cell pixels of the 41 px run that the
default run (5642630 code, `premise/default`) draws as voxel trees, 2 px eroded; lit/shaded
by one Otsu threshold on the default run's sun view. Mean OKLab L / C / h:

| commit | a1 lit | a1 shaded | h1 lit | h1 shaded | far field ms (a1, 41 px) |
|---|---|---|---|---|---|
| cluster voxels (default run) | 0.382 / 0.033 / 220 | 0.268 / 0.069 / 250 | 0.304 / 0.023 / 209 | 0.168 / 0.049 / 254 | — |
| tiles before (5642630) | 0.509 / 0.033 / 204 | 0.372 / 0.072 / 248 | 0.382 / 0.029 / 198 | 0.230 / 0.056 / 249 | 8.72 |
| 01e57f2 leaf lobe | 0.445 / 0.039 / 209 | 0.337 / 0.075 / 248 | 0.339 / 0.033 / 202 | 0.205 / 0.056 / 250 | 7.77 |
| 08ea239 sky up (sides variant: 0.419 / 0.302 a1, 14.74 ms, dropped) | 0.428 / 0.038 / 209 | 0.313 / 0.076 / 249 | 0.328 / 0.032 / 202 | 0.191 / 0.056 / 252 | 9.02 |
| 336e429 sun march (from the hit: 0.377 / 0.303, lit share 130k vs 160k, dropped) | 0.403 / 0.042 / 221 | 0.304 / 0.071 / 248 | 0.307 / 0.035 / 216 | 0.201 / 0.055 / 250 | 12.05 (14.58 with two call sites) |
| 892b979 palette | 0.405 / 0.039 / 227 | 0.304 / 0.071 / 249 | 0.315 / 0.030 / 221 | 0.201 / 0.055 / 251 | 12.06 |

Noise: two launches of 892b979 (`palette`, `pal-full`) give the same tile means to 1e-4;
cluster-voxel side of the switch line across launches 0.3745-0.3764 lit (a1).
Debug split on the same pixels, a1 (`split.py`): base L 0.341 h 128 voxels, tiles 0.309 h 139
before -> 0.330 h 131; sun 0.316 voxels, 0.421 -> 0.289; ambient 0.245 voxels, 0.412 -> 0.325
(h1 alike). Pure-move check 0ca091c: a1 default vs pre-move, voxel-tree L 0.28832 / 0.28833,
largest 4x4 |dL| 0.00004, whole-frame diff 0.2444.

Default switch, `.superpowers/far/cap.sh <bin> A1|H2|H1 .superpowers/far/s3/final/<v>.png`,
after = build-release 892b979, before = `.superpowers/far/stage1` worktree at c607a93:
- a1: `... voxel trees 5.77, far field 8.95, ...`, `Rendered 120 frames, GPU 29.98 ms` (before: far field 6.46, GPU 27.38)
- H2: `voxel trees 6.64, far field 3.08`, `GPU 28.69 ms` (before 2.20, 27.72)
- H1: `voxel trees 6.06, far field 8.99`, `GPU 30.66 ms` (before 6.56, 28.16)
- `Loaded: resident 9927 MiB` all six (+72 MiB far window = 9999 MiB); palette 256 x 32 B.
- Whole-frame diff before/after: a1 0.507, H2 0.482, H1 2.835 (noise a1 0.24).
- Debug a1 `--frames 2` (`final/a1-debug.log`): `far field hash 0x18e4df10c8f99dce, placement grid inside its outer ring+1 cells 0x95b68fb05960ae2d` (albedo bytes changed by the palette, format unchanged), `far field check: 13 cells, 35072 voxels (18650 occupied) ... 0 mismatches`, far placement / shadow map / ray checks as before, `0 validation errors`.
- Load: `templates 13 ms, splat 279 ms` (was 9 / 255).
Look: `.superpowers/far/s3/final/sheet-stage3.png`, `.superpowers/far/s3/final/crop-h1.png`,
`.superpowers/far/s3/premise/crops.png` (41 px switch, before).
