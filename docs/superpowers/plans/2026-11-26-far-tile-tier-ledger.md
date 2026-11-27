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

| sky from the hit (director ruling (a), `measure.sh skyhit`) | 0.386 / 0.039 / 227 | 0.280 / 0.072 / 250 | 0.303 / 0.030 / 222 | 0.186 / 0.055 / 253 | 12.21 |

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

## Stage 3b sheet (director's first small task), 4772a79

`.superpowers/far/cap.sh build-release/game/game A1|H2|H1 .superpowers/far/s3/final3b/<v>.png`
(default switch, build-release at 4772a79), each `Render 2560x1440, display 2560x1440, jitter phases 8, mip bias 0, DLSS`,
`Loaded: resident 9927 MiB` (+72 MiB far window), 0 validation errors:
- a1: `cull clusters 4.19 ... voxel trees 5.87, far field 9.00`, `Rendered 120 frames, GPU 30.15 ms`, main voxel quads 16709735
- H2: `cull clusters 5.11 ... voxel trees 6.59, far field 3.10`, `GPU 28.72 ms`, voxel quads 21010977
- H1: `cull clusters 4.38 ... voxel trees 5.87, far field 9.03`, `GPU 30.58 ms`, voxel quads 19025468
Sheet: `.superpowers/far/s3/final/sheet-stage3b.png` (a1/H2/H1, then 5.jpg / 7.jpg).

## Stage 2 part 1: fine levels over a static window (`--far-fine`)

Decisions with reasons: decisions.md "Far tile tier stage 2, part 1".
Need (a1, `--far-fine 1024`, window 33x33 / 65x65 cells): `Far field 1 m: 2748590 bricks (63430147 occupied voxels) ... bricks 273.4 MiB`,
`Far field 2 m: 1656278 bricks ... bricks 206.0 MiB` (before the fine bands were limited to the crop).
Whole crop (`--far-fine 3700` at a1, 64x64 cells): `Far field 1 m: 11459524 bricks (281231435 occupied voxels) in 983590 columns, 64 slices; bricks 1204.0 MiB, columns 8.0 MiB; splat 632 ms`,
`Far field 2 m: 1653111 bricks (49711928 occupied voxels) ... bricks 208.6 MiB`; `Loaded: resident 11340 MiB` (+1413 MiB over 4772a79; +72 MiB far window = 11412 MiB).

Switch band, same-tree pixels (`VIEWS=.superpowers/far/s3/views-measure.txt .superpowers/far/s3/views.sh build-release/game/game .superpowers/far/s2/<dir> --far-fine 3700`,
`python3 -I canopy.py premise/default ../s2/<dir> a1|h1`), L lit / shaded, `Render 2560x1440`, 0 validation errors:

| run | a1 lit | a1 shaded | h1 lit | h1 shaded | far field ms a1 |
|---|---|---|---|---|---|
| cluster voxels (default) | 0.381 | 0.267 | 0.306 | 0.175 | — |
| 1 m tiles, sun/sky at own level (`s2/fine`) | 0.354 | 0.266 | 0.276 | 0.196 | 20.91 |
| 1 m tiles, sun/sky from 4 m (`s2/fine-shade4`, kept) | 0.369 | 0.266 | 0.291 | 0.189 | 16.51 |

Cost, 4772a79 default vs `--far-fine 3700` (fine-shade4):
- a1: voxel quads 16709735 -> 12730153; cull clusters 4.19 -> 2.83; voxel trees 5.87 -> 4.75; far field 9.00 -> 16.51; `GPU 30.15 ms` -> `GPU 34.67 ms`.
- h1: voxel quads 19025468 -> 16002194; cull clusters 4.38 -> 3.98; voxel trees 5.87 -> 5.32; far field 9.03 -> 12.95; `GPU 30.58` -> `GPU 33.23 ms`.

Debug a1 `--far-fine 3700 --frames 2` (`.superpowers/far/s2/a1-debug.log`):
`far field hash 0x18e4df10c8f99dce, placement grid inside its outer ring+1 cells 0x95b68fb05960ae2d (static levels); fine levels 0xc0a1e97bc3a0eee6`
(static hashes equal 892b979/4772a79's), `far field check: 13 cells, 787584 voxels (334557 occupied) compared with the CPU replica, 0 mismatches`
(3 crop cells at the fine levels), placement / shadow map / ray checks as before, `0 validation errors`. Tests: 221 passed.
Look: `.superpowers/far/s2/crop-switch-a1.png` (1:1 switch band, a1, beside 7.jpg), `.superpowers/far/s2/sheet-fine.png`.

Director ruling (C), 2026-11-27: fine levels stay as an A/B, streaming deferred. Default check at de862ea:
`.superpowers/far/cap.sh build-release/game/game A1 .superpowers/far/s2/a1-default.png`: `Render 2560x1440 ... DLSS`,
`Rendered 120 frames, GPU 30.19 ms` (4772a79 30.15), `cull clusters 4.16 ... voxel trees 5.82, far field 9.04` (4.19 / 5.87 / 9.00),
main voxel quads 16709735 (equal), `Loaded: resident 9927 MiB` (equal), 0 validation errors;
`python3 -I tools/image-gates.py diff .superpowers/far/s3/final3b/a1.png .superpowers/far/s2/a1-default.png`: `diff 0.2440` (noise a1 0.26).

## Hardening (2026-11-27, build of 37812f2; code unchanged since 466a760 apart from comments)

Scene flags `$SCENE` = `.superpowers/far/flags.sh`. Every run `Render 2560x1440, display 2560x1440, jitter phases 8, mip bias 0, DLSS`.

a. Fly-through, route `.superpowers/far/fly/fly.route` (1500 m, yaw +x, 100 m/s: x 0 -> 2000 "core", 2000 -> 5000 "beyond-edge"):
`flock /tmp/meridian-gpu.lock ./build-release/game/game $SCENE [--far-extent 0] --benchmark 50 F.csv --route fly.route`.
- far-tier: `Recorded 2433 frames: AVG 48.7, 0.1% low 26.8, 1% low 29.2, 5% low 33.7, MAX 57.3 FPS`, `0 dropped`, `0 validation errors`.
  bench-route: `core 855 42.8 27.3 23.38 far_field 10.28`, `beyond-edge 1577 52.6 44.5 19.02 far_field 8.54`.
  Maxima (`.superpowers/far/fly/maxima.py`): `gpu_ms: max 38.22 at frame 110 t 3.10s` (far field 15.71, voxel trees 7.31), `far_field: max 16.31`, p99 13.88.
- `--far-extent 0`: `Recorded 4913 frames: AVG 98.2, 0.1% low 40.0, 1% low 49.7`; `core 12.29 ms`, `beyond-edge 9.13 ms`; `gpu_ms: max 27.53`.
- Terrain shadow builds, both runs: near window 1.28-1.36 ms every 1 km; far window `(1882, 40.07, 3.11)` at x ~4000 (rebuild distance 4 km), one frame.
- Frames `.superpowers/far/fly/views/` (x1000..x5000, x5000-back + lod, H1 at 5 degrees sun): no holes (lod: every non-meadow cell cyan);
  past x = 4096 the camera is in the 8 m band: at x5000 looking back the ground below is 8 m blocks (backlog row).
  H1 low sun (`--sun 0.704 0.0872 -0.704`): `GPU 29.76 ms`, `far field 7.59`, `0 validation errors`.

b. Motion (`.superpowers/far/motion/motion.sh`, `table.sh`; side 30 m/s, yaw 0.25 rad/s; jitter = frame 120 vs 121, far band a1 rows 0.15-0.28, H1 0.35-0.49):
`--far-extent 0` draws `far cells 0` at a1 and H1 (main's far field draws none there either, stage 0), so it is the main-equivalent.
- A1 far band jitter far / ext0: still 0.44 / 0.42, side30 1.33 / 1.29, turn 1.31 / 1.29.
- H1 far band jitter far / ext0: still 0.45 / 0.39, side30 0.92 / 0.73, turn 1.04 / 0.81 (ext0 shows bare terrain there).
- Still runs: `far/A1_still_120.log` `GPU 30.42 ms`, far field 9.48; `ext0/A1` `GPU 20.98 ms`; H1 30.55 / 21.63.

c. Default scene (`./build-release/game/game --trees --capture-views .superpowers/far/default/views.txt DIR`; main = 363096d built in `.superpowers/far/main`):
- far-tier: `Far field: resident 1376 MiB; peak transient 1166 MiB, device memory allocated at peak 12454 MiB`, `Loaded: resident 12025 MiB`,
  start `GPU 24.79 ms` far field 0.68, summit horizon `GPU 32.59 ms` far field 1.55 (far cells 40186), 0 dropped, 0 validation errors.
- main: `resident 731 MiB ... at peak 11782 MiB`, `Loaded: resident 11370 MiB`, start `GPU 23.97 ms`, summit horizon `GPU 31.67 ms` far field 0.04 (far cells 470).
- `image-gates.py diff`: start 0.63, summit horizon 1.24 (far vs main); `--far-extent 0` vs main 1.02, far vs `--far-extent 0` 1.27;
  far-tier is ~1/255 brighter everywhere (summit view mean 109.57 vs 108.60), no local far-field artifact.

d. Debug (`.superpowers/far/cap.sh build/game/game H1|H2`, `.superpowers/far/debugcap/`): both `0 validation errors`,
`far field hash 0x18e4df10c8f99dce`, far field / far placement / stream checks `0 mismatches`, `far shadow map check ... max difference 0.248 m (0 over 1 m)`,
`far field ray check ... max difference 8.7e-07`; the 10 known warnings (outputs 4/5, backlog).

Sheet: `.superpowers/far/sheet/sheet-final.png` (a1/H2/H1 far-tier, `--far-extent 0`, 5.jpg/7.jpg); far pass a1 9.05, H2 3.07, H1 9.10 ms; `Loaded: resident 9927 MiB`.

## Review fixes and the cell-border lines (2026-11-27; commits 31cfa1b, 8263fc7, 2df00e8, 67d3fa7, bd2ec08)

Scene flags `$SCENE` = `.superpowers/far/flags.sh`; captures `VIEWS=FILE .superpowers/far/s3/views.sh BIN DIR`; every run
`Render 2560x1440, display 2560x1440, jitter phases 8, mip bias 0, DLSS`, `0 validation errors`.
Before = f98f774 (build-dev, or the hardening frames `.superpowers/far/fly/views/`, same code).

Grid lines (fly frames). Light split at x4000 (`lines/before/`): base colour flat at the borders, sun and ambient not.
Border profile (`lines/profile.py`: mean linear luminance at -8..8 px across the far-cell changes of the per-cell hue
lod view `lines/cells/x4000-cells.png`, debug_view.frag patched locally, not committed; normalised to |d| >= 5):
- before `x4000.png 0.97 0.97 0.97 0.97 0.97 0.96 0.94 0.90 0.99 1.02 1.03 1.04 1.04 1.04 1.03 1.03 1.02` (lit step + dark line)
- sun march across cells only (`lines/after1`; step gone, dark line left): `after1/x4000.png 1.01 1.00 1.00 1.00 0.99 0.98 0.96 0.91 0.95 0.97 0.98 0.99 0.99 1.00 1.00 1.00 1.00`
- plus the carried view march (`lines/after2`, final `lines/final`): `final/x4000.png 1.00 1.00 1.00 1.00 1.01 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 1.00 0.99 0.99`
- Sheet 1:1: `.superpowers/far/lines/sheet-lines.png` (x3000, x4000, x5000, x5000-back; before | after).

Sun lookup leak (review item 7): march to 32 m + far shadow texel + bias, lookup at 32 m (`review/leak`): far tiles' sun at x4000
`mean 0.03494 -> 0.03351 ratio 0.9591`, H1 at a 5 degree sun `ratio 0.9926` (whole frame). Committed fix: lookup pulled back instead.

Edge strip (review item 1), `--camera 2000 1500 -1000 3.1416 -0.45` (`review/{before,after}/edge{,-lod}.png`): a bare
grey strip along x = 2000..2048 before, voxel trees across it after.
Far level (review item 2), `--camera 5000 1500 0 1.5708 -0.5` (`review/{before,after}/far6k{,-sun,-ambient}.png`):
ambient view `far6k-ambient mean before 0.04143 after 0.03518 ratio 0.8492 lower half 0.6148`.

Debug H1 (`.superpowers/far/cap.sh build/game/game H1`, `review/debug-h1.log`): `0 validation errors` (the 10 known warnings),
`placement hash 0x9f383bc0acab499b` (was 0x212a5bc72625e8fe: the edge strip's trees), `placement check: 5425 points (762 kept) ... 0 mismatches`,
`far field hash 0x944195d140d56146, placement grid inside its outer ring+1 cells 0x95b68fb05960ae2d (static levels)` (static hash was
0x18e4df10c8f99dce; the interior one is unchanged), far field / far placement / stream checks `0 mismatches`,
`far shadow map check ... max difference 0.248 m (0 over 1 m)`, `far field ray check: 256 rays (52 partly transmitted) ... max difference 8.7e-07`.

Release after (`lines/final/run.log`): x4000 `GPU 22.11 ms` far field 11.76; a1 `GPU 34.31 ms` far field 12.64; h1 `GPU 34.39 ms` far field 12.31
(hardening: a1 9.05, h1 9.10): the marches across cells cost ~3.4 ms at a1; task 2 next.

## Far pass cost after the border fix (2026-11-27; commit 4504403)

Probes: `.superpowers/far/cost2/` (`voxel.frag` + `lighting.glsl` copies with `PROBE_*` defines, `probe.sh NAME DEFINES...`
compiles into build-dev and captures a1 + H1; `probe-engine.sh` the same from engine/shaders). Base = 2df00e8's shader.
Far field ms a1 / H1 (each from the run's `GPU passes:` line; every run `0 validation errors`, `Render 2560x1440`):

| probe | a1 | H1 |
|---|---|---|
| base / base2 (repeat) | 12.58 / 12.63 | 15.08 (outlier) / 12.38 |
| no carried march (PROBE_NOPREFIX) | 10.98 | 11.14 |
| no shading at all (PROBE_NOSHADE: march, raster, discard) | 5.10 | 4.77 |
| no sky / sun marches | 8.69 | 8.63 |
| no sun lookup (traceSunVisibility) | 13.93 | 10.00 (noisy) |
| no ambient | 10.22 | 9.87 |
| no dead fallback march in traceSunVisibility | 12.60 | 12.40 |
| carry 2 voxels | 11.81 | 11.67 |
| sky 16 m, sun 8 m | 11.68 | 11.68 |
| skip sun march where the lookup is 0 | 11.50 | 11.22 |
| skip + carry 2 + sky 16 / sun 8 | 9.71 | 9.79 |
| skip + carry 2 + sky 8 m / sun 4 m | 10.47 | 10.48 |
| skip + carry 2 + sky 4 m / sun 8 m | 10.06 | 10.06 |
| one march for view, sky, sun (`try/unified.diff`) | 17.78 | 16.58 |
| surface after the marches | 12.65 | 12.40 |

Frame diff vs base2 (`tools/image-gates.py diff`; noise base vs base2: a1 0.2427, H1 0.1966): skip a1 0.2433 / H1 0.1962;
carry2 0.2504 / 0.2264; sky16/sun8 0.2852 / 0.4424 (region means within 0.3%); sky8 0.2686 / 0.3003; sun8 0.2667 / 0.3227.

Kept (skip + carry 2), build-release, `cost2/final`: a1 `GPU 31.91 ms` far field 10.64, H1 `GPU 32.44 ms` far field 10.62,
x4000 `GPU 21.16 ms` far field 10.64. Diff vs `lines/final` (2df00e8): a1 0.2718, H1 0.2321. Border profile x4000:
`cost2/final/x4000.png 1.00 1.00 1.00 1.00 1.00 1.00 0.99 0.98 0.99 0.99 0.99 0.99 1.00 1.00 1.00 0.99 0.99`.
Sky march in the view loop (`try/sky-in-view-loop.diff`): 10.43 / 10.49, diff 0.2431 / 0.1966: dropped (noise-level gain).

Tile table at the forced switch (`--far-switch-px 4`, `cost2/sw-final` vs `cost2/sw-base`, `canopy.py cost2/default ...`), L lit / shaded:
a1 tiles 0.3836 / 0.2790 (base 0.3843 / 0.2797), voxels 0.3811 / 0.2669; switch line tile side 0.3758 / 0.2737 (base 0.3768 / 0.2749);
H1 tiles 0.3050 / 0.1938 (base 0.3058 / 0.1941), voxels 0.3149 / 0.1818; line 0.3114 / 0.1968 (base 0.3123 / 0.1974).

Fly-through (`$SCENE --benchmark 50 cost2/fly/fly.csv --route .superpowers/far/fly/fly.route`):
`Recorded 2108 frames: AVG 42.1, 0.1% low 24.3, 1% low 27.4, 5% low 30.6, MAX 50.0 FPS`;
bench-route `core 734 36.7 26.0 27.23 far_field 13.60`, `beyond-edge 1373 45.8 37.5 21.86 far_field 11.42`, `route 2107 42.1 27.4 23.73 far_field 12.18`;
maxima `far_field: max 25.32 at frame 512 t 14.14s, mean 12.18, p99 23.26`, `gpu_ms: max 43.43 at frame 514 t 14.22s, mean 23.67`.

## Final checks (2026-11-27, build of 4504403)

Default Napf scene (`flock /tmp/meridian-gpu.lock ./build-release/game/game --trees --capture-views .superpowers/far/default/views.txt .superpowers/far/final/default`, log `final/default.log`):
`Placed 3398673 instances` (before the edge-strip change 3364956), `Far field: resident 1382 MiB; peak transient 1172 MiB, device memory allocated at peak 12460 MiB`
(before 1376 / 12454), `Loaded: resident 12033 MiB` (before 12025); start `GPU 25.18 ms` far field 0.82, summit horizon `GPU 33.03 ms` far field 1.85;
every view `0 validation errors`, every cull list `0 dropped`.
Fly spike pose (`final/spike`, `--camera 1414 1500 0 1.5708 -0.12`): `GPU 24.90 ms`, far field 14.03, 0 validation errors; lod view: tiles over ~75% of the frame,
camera far above the canopy. The fly csv's 25.3 ms blocks (t 14.1-16 s) are not reproduced still (backlog row).

## Fix check of the review fixes (2026-11-27; independent check of 18f280e)

Runs: `VIEWS=.superpowers/far/fix2/views.txt .superpowers/far/s3/views.sh build-release/game/game .superpowers/far/fix2/<run> --far-switch-px 4`
(a1, h1 plain / lod / sun, x4000-lod, x5000-lod; tiles from ~1.8 km), `before` = 6c97137, `after` = the fix commits, `nocarry` =
6c97137's voxel.frag with the carried march off (`fix2/voxel.frag.nocarry` into build-dev). Every view `Render 2560x1440`,
`0 validation errors`, `0 dropped`.

Sun hand-off (item 7). Far tiles' sun debug view, mean OKLab L over the tile pixels both runs share
(`python3 -I .superpowers/far/fix2/sunmean.py fix2/before fix2/after a1|h1`):
`a1: tile px 395515, sun L all 0.2596 -> 0.2578 (0.9933x), lit half 0.4204 -> 0.4192 (0.9973x)`;
`h1: tile px 557114, sun L all 0.3277 -> 0.3274 (0.9990x), lit half 0.4108 -> 0.4107 (0.9995x)`.
Noise (same code, `cost2/sw-final` vs `fix2/before`): `a1 ... 0.2596 -> 0.2596 (1.0000x)`, `h1 ... 0.3277 -> 0.3277 (1.0000x)`.
Tile table (`python3 -I .superpowers/far/s3/canopy.py cost2/default fix2/<run> a1|h1`), L lit / shaded, tiles before -> after vs voxels:
a1 0.3836 / 0.2790 -> 0.3833 / 0.2787 vs 0.3808 / 0.2669 (both toward the voxels); h1 0.3050 / 0.1937 -> 0.3049 / 0.1935 vs
0.3149 / 0.1817 (shaded toward; lit 0.0001 away, below the final frame's noise). Whole frame diff before/after a1 0.2756, h1 0.2149
(noise 0.26 / 0.20; the after run carries the pinhole fill too).

Mirror policy (item 1 test gap). `./build-release/tests/tests`: `225 passed | 0 failed`. Mutation check: `placementMirrors` returning
true always fails `CHECK( strip(PlacementKind::Streamed) == 0 )`, false always fails `CHECK( strip(PlacementKind::Static) > 0 )`.
Debug H1 (`.superpowers/far/cap.sh build/game/game H1 .superpowers/far/fix2/final/debug-h1.png`): `placement hash 0x9f383bc0acab499b`
(unchanged), `placement check: 5425 points (762 kept) compared with the CPU reference, 0 mismatches (height within 0 m)`,
`far field hash 0x944195d140d56146, placement grid inside its outer ring+1 cells 0x95b68fb05960ae2d (static levels)` (unchanged),
`far placement check: 6318 points (3661 kept) in the sample windows compared with the CPU reference, 0 mismatches`.
