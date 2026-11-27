# Far lane: resume point (2026-11-27, after stage 3 tile shading)

Worktree `/home/zye/Projects/dev/meridian-far`, branch `far-tier`, pushed (merge ed758d8 + this RESUME commit).
Read: CLAUDE.md, spec `docs/superpowers/specs/2026-11-26-far-tile-tier-design.md`,
plan `docs/superpowers/plans/2026-11-26-far-tile-tier.md`, ledger
`docs/superpowers/plans/2026-11-26-far-tile-tier-ledger.md` (every number with its raw line).

## State
- Stage 0 and stage 1 are done and accepted. Plan boxes 1.x are still unticked: tick 1.1-1.7 when stage 2 lands.
- f9af968: interior crop hash. Cause: the splat measured positions from the far grid's corner, so float rounding moved deposits. Fix: `FarFieldGrid::anchorX/Z` (CPU, GPU, replica windows) plus a unit test. The hash now equals main's (0x35d39cce445d2399). Reported to the director.
- 2e9b54e: far window of the terrain shadow-height map (3072^2 x 16 m). Far pass 18.24 -> 6.43 ms at a1, 19.18 -> 6.55 at H1, 2.21 at H2. The near-to-far band draw was tried with no gain and reverted (`.superpowers/far/cost/with-bands.diff`).
- 8065654: far shadow box limited to the L1 band (director ruling b): 8 m texels.
- b6a4dfb: stale decisions.md line fixed.
- Director ruling (2026-11-27): 6.4 ms accepted. No shade-once pass (the visibility buffer is its planned home); the 3072^2 far window stays. Go to stage 2.
- ed758d8: merged origin/scene-broadleaf 97bd2ef (main 363096d crown-specular, round-2c ground colours). Only decisions.md conflicted (both kept). Release a1 after the merge (`.superpowers/far/cost/merge-a1.log`): `Render 2560x1440 ... DLSS`, `far field 6.50`, far cells 60050, 0 dropped, 0 validation errors, `Loaded: resident 9927 MiB`.
- The remaining far pass cost is ~2.5 ms march/raster and ~3.9 ms shading (backlog row).

## Stage 3 (done, reported to the director; director had moved it before stage 2)
- c607a93 plan order; 0ca091c pure move of the cluster voxels' sun/ambient into
  lighting.glsl `addVoxelFoliageSun` / `addVoxelFoliageAmbient` (kept separate for the
  crown-light lane's hand merge: it adds an in-crown fill to voxel_instance.frag; the tile
  needs that fill too once it lands, from `depths.y` in voxel.frag, do not invent one);
  01e57f2 leaf lobe (per-level mean alignment, GpuFarField::alignment); 08ea239 sky up 64 m
  through the cell's bricks (canopy-sky form); 336e429 sun march one voxel out to 32 m, then
  maps from the march end (backlog double self-shadow row removed); 892b979 per-voxel
  palette (256, median cut); 0a41b69 ledger; fe1d694 backlog.
- Director accepted stage 3. Ruling (a) done: sky march from the hit (see the latest
  commit): a1 lit/shaded 0.386/0.280 vs voxels 0.382/0.268, h1 0.303/0.186 vs 0.304/0.168
  (h1 shaded 0.018 above the director's ~0.015; every gap closed, lit moved toward the
  voxels; kept, flagged to the director, a one-line revert if overruled). Cost accepted
  (+2.5 ms, no kTileSunMarch tuning). Crown-light double count and the tile in-crown fill
  are settled at the merge, not now (the leaf lane is still changing the fill).
- Measurement kit: `.superpowers/far/s3/{views.sh,measure.sh,canopy.py,split.py,regions.py}`;
  default reference frames `.superpowers/far/s3/premise/default`.
- `.superpowers/far/stage1`: local worktree at c607a93 with build-release (before frames).

## Next
1. Stage 2 (plan 2.1-2.5): streamed 2 m and 1 m levels, the switch by voxel pixel size.
   Report must include: fixed pools sized from the measured need; a cell always has something
   to draw; 1:1 crop of the switch band beside 7.jpg; whole frames a1/H2/H1; true resident
   VRAM (`Loaded:` + 72 MiB far window); frame-time maxima over a fly-through from the core
   across the crop edge; stream pass ms; the far window rebuild cost on a moving camera and a
   low-sun run. Proof at the switch band (director): the same lit/shaded table, tiles vs voxels
   of the same trees (`.superpowers/far/s3/canopy.py`), plus the 1:1 crop and the motion run
   across the switch.

## Tools
- `.superpowers/far/cap.sh BIN VIEW OUT.png [flags]` (flock inside; views A1 H2 H1 in flags.sh).
- `.superpowers/far/cost/runv.sh BIN TAG [flags]`: a1 + H1 captures plus their pass lines.
- `.superpowers/far/cost/final.sh`: a1/H2/H1 Release + Debug a1 checks.
- `.superpowers/far/sheet.py OUT --row LABEL imgs... --crop LABEL img x y w h ...`.
- `python3 -I tools/image-gates.py diff a.png b.png`: whole-frame mean difference. Noise: a1 0.26, H1 0.20 per 255.
- Builds: `build-release` (captures), `build-dev` (Release, for variants; currently the committed code), `build` (Debug).
- Base at ffc505b in `.superpowers/far/base` (local only; its renderer_farfield.cpp carries the interior-hash patch; its `build` binary carries a FAR_DUMP per-column dump and segfaults without that env var, so rebuild it before reuse).
- Shader variants for cost probes: `.superpowers/far/voxel.frag.{orig,noterrain,noshade}` (orig = before 2e9b54e; voxel.frag itself is unchanged since).
- DLSS SDK: `-DFETCHCONTENT_SOURCE_DIR_DLSS_SDK=/home/zye/Projects/dev/meridian/build-release/_deps/dlss_sdk-src`.
