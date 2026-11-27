# Far tile tier — plan

Status: in progress (stage 3; order changed by the director 2026-11-27:
stage 3 before stage 2, since the far canopy's flat, light, grey shading
is the larger whole-frame gap and the smaller job). Spec:
`specs/2026-11-26-far-tile-tier-design.md`. Branch `far-tier`, worktree
`../meridian-far`. No merge to main: the user judges the result by eye.

Ledger: `plans/2026-11-26-far-tile-tier-ledger.md` (create it): every
command, its raw output line, the commit hash and the `Render WxH` line.
Artifacts (frames, sheets, logs): `.superpowers/far/` in the worktree,
never `/tmp`.

Scene flags for every capture (the current look):
`--terrain assets/terrain/relief-0.5.height --trees --tree-palette
chlorophyll1 --scenery broadleaf --haze clean --sun 0.707 0.700 -0.707
--grade <grade json>`; views a1 `--camera 1300 1350 1300 -0.785 -0.25`,
H2 `850 1023.2 750 -0.785 0`, H1 `-875 1435 -900 3.1416 -0.12`. The
palette, scenery and grade flags live on `scene-broadleaf`
(`../meridian-scene`, 13469bf): check which exist on this branch first; if
they are missing, merge `scene-broadleaf` into `far-tier` before stage 1
(it merges main regularly) and say so in the ledger.

Rules for every task: premise first (read the cited code or take the one
capture that can disprove the idea before changing code); one try plus one
variant, then report; every game run through `flock /tmp/meridian-gpu.lock`;
numbers only at 2560x1440; no long Debug runs (Debug only for the short
replica checks); commits without AI attribution, one feature per commit;
docs in the same commit as the change.

## Stage 0: premise (no structural change)

- [x] 0.1 Read `cell_cull.comp` `farSwitch`, `farfield.glsl` (switch
  constants, `farMarchCell`), `voxel.frag`, `renderer_farfield.cpp`
  `buildFarField`. Note in the ledger anything that contradicts the spec's
  "Meridian today".
- [x] 0.2 `--far-switch-px N`: the run's value for `kFarSwitchPixels`
  (scene field, default unchanged). cli.md line.
  Verify: default run's frame equals main's (mean difference about 1e-6).
- [x] 0.3 a1 and H1 at the default, and with N chosen so the switch sits
  where the 4 m level is 1 px and 2 px. Record voxel quads, voxel pass ms,
  far pass ms, `far cells N`. Sheet: the three frames per view beside
  7.jpg, plus one 1:1 crop of the switch band.
- [x] 0.4 Report to the director: saving available, look gap at the
  switch, anything false in the spec. Continue with stage 1 without
  waiting unless a premise failed.

## Stage 1: forest to the horizon

- [ ] 1.1 Mirrored rule evaluation. `placement.cpp` (`insideCrop`,
  `evaluatePoint`, geo channel and parent-density reads) and
  `placement.glsl` (`placementInsideCrop` and the same reads) get one
  integer fold of a position into the crop (mirrored repeat, matching
  `terrain.cpp` heights), used only when the caller asks for far cells.
  Unit test: fold is its own inverse on the crop, matches the heightfield's
  mirror at tile seams, CPU and GPU hash of placed points equal for sample
  outside cells (Debug replica).
- [ ] 1.2 Far grid. `FarFieldGrid` covers `farExtent` (a renderer setting,
  default the fixed-point limit; a flag `--far-extent KM` for A/B) instead
  of `placementGrid`; per cell the finest static level it stores (band by
  distance from the crop: 4 m to `kFarBand4`, 8 m to `kFarBand8`, 16 m
  beyond; named constants with the pixel reasoning). `FarCell` or the
  field carries that level; `farSwitch` clamps to it. Everything that
  indexes far cells by placement cell index (cell_cull, voxel.mesh/frag,
  gi_probes, far shadow, `cellInTlas`) is listed first, then moved to the
  far grid's own index. Verify: inside the crop the bricks are bit-equal to
  main's (the Debug hash line).
- [ ] 1.3 Transient instances. The splat's input for cells outside the
  crop comes from `placement.comp` run on those cells in slices (same
  slices as the splat's rows), into a transient buffer that is freed after
  the level's pack. Nothing reaches `placedInstances`, the cell table or
  the TLAS. Verify: instance count, TLAS size and `Loaded: resident` minus
  the far field unchanged against main.
- [ ] 1.4 Draw. Cells outside the crop always draw (no tree fallback) at
  `max(level by pixels, finest stored)`. Cull list sizes for far tasks from
  the measured need over the run, shadow views included.
- [ ] 1.5 Far shadow map and cascade 4 over the extended grid:
  `farShadowGrid` box from the non-empty far cells (already capped by
  `maxTexels`); confirm the cap and the texel size it lands on, report it.
- [ ] 1.6 Downstream sweep: grep for every use of `placementGrid`,
  `cellsX`/`cellsZ`, `cellGrid`, "crop" in shaders and docs that assumed
  far grid == placement grid; fix or list.
- [ ] 1.7 Captures a1, H2, H1 (whole frames beside 5.jpg and 7.jpg, same
  sheet, reference row). VRAM lines, far pass ms, startup time delta.
  Debug: one short capture for validation and the replica check.
  Report to the director. This is the first frame pair for the user.

## Stage 2: streamed fine levels and the switch (after stage 3)

Status: part 1 done (de862ea: fine levels over a static window,
`--far-fine`, default off; switch by finest built voxel), streaming
deferred (director 2026-11-27: inside the crop trees are cheaper than
fine tiles; spec decisions 3 and 4). 2.1 and 2.3 done, 2.2 deferred, 2.4
done for the static window, 2.5 measured at a1/h1 (ledger).

- [ ] 2.1 Levels 2 m and 1 m as streamed levels: `kFarLevels` grows to 5
  with the two fine levels first or last (choose by least churn; the level
  index packed into the far entry's top bits needs 3 bits: find every
  `<< 30`). Templates for the fine levels (`filterFarTemplate`; the 1 m
  level is the bake grid itself).
- [ ] 2.2 Stream window per fine level: cells within the distance where
  the level is the drawn one, plus a margin of one cell; fixed pools sized
  from the measured need at the three views plus a fly-through (state the
  need, then the cap); slot table like `streamSlot`. Per frame: cells
  entering the window are splatted within `kFarStreamBudget` (named, bricks
  or ms per frame), cells leaving free their bricks. The occupancy
  readback of the load-time splat cannot stall a frame: allocate from a
  GPU-side upper bound or read back a frame late; choose after reading the
  code and say why.
- [ ] 2.3 Switch rule (spec decision 4) in `farSwitch`: a cell is far when
  a level it has built projects to at most `kFarLevelPixels`; inside the
  crop an unbuilt cell keeps its trees. The 6 px tree rule goes.
  Shadow views: decide per cascade from texel size as today; state the rule.
- [ ] 2.4 Debug replica check extended to streamed cells (sample a few
  after the window settles).
- [ ] 2.5 Measure: voxel quads, voxel pass, far pass, stream pass ms at
  a1/H1/Part B; frame-time maxima over a fly-through from the core across
  the crop edge; VRAM. Frames and a 1:1 crop of the switch band beside the
  reference; motion run across the switch (`tools/motion-clarity.sh` with a
  view pair that straddles it). Report to the director.

## Stage 3: tile shading and shadows (now; before stage 2)

Proof (director 2026-11-27): with `--far-switch-px 41` (tiles from about
1.8 km, 4 m level) at a1 and H1 the tiles match the cluster voxels of the
same trees in a default-switch run of the same view: mean OKLCh L / C / h of
lit and of shaded canopy measured separately, noise floor stated. Then
whole frames a1/H2/H1 beside 5.jpg and 7.jpg and the 1:1 H1 horizon crop
beside the 7.jpg crop, before (stage 1) and after; far pass ms and VRAM.

- [x] 3.1 Colour: the palette holds every colour the templates carry (per
  template voxel: its dominant material's mean colour at 1 m, as the
  cluster voxels have it), not one entry per mesh slot; premise: compare a
  tile crop against the same trees as cluster voxels in the views'
  base-colour debug view.
- [x] 3.2 Sun through the volume: a short march toward the sun through the
  cell's bricks from the hit (optical depth, named step count), replacing
  the fixed `kVoxelRayStart` offset; then the cascade or far shadow map
  with the matching bias. Removes the double self-shadow (backlog row
  "Cascade 4 casts placed trees as far-cell voxels"; done, 3.2 commit).
- [x] 3.3 Sky occlusion from the volume for the ambient (today
  `s.occlusion = 1.0`): the same form as the crown-light lane's
  `--canopy-sky up` (branch `crown-light` 63c3751, not merged): the
  transmittance straight up over 64 m of the cell's bricks, applied to the
  ambient's out-of-clipmap share, so tiles and cluster voxels agree once
  both land.
- [x] 3.4 Sheet beside the reference at the switch band and whole frame.
  Report.

## Stage 4: close-out

- [ ] 4.1 Docs: architecture.md (far field section, frame order if a pass
  was added), cli.md, decisions.md entry "Far tile tier" (and fix the stale
  far-field statements: switch rule, template source, VRAM figure),
  backlog rows removed or rewritten, resume.md.
- [ ] 4.2 Reference sweep: every doc and comment that states the old switch
  rule, "three levels", "covers the crop" or the old VRAM figure.
- [ ] 4.3 Independent Opus review in a fresh window and worktree, given the
  diff and the spec's acceptance list, not this plan's ledger. Fix check.
- [ ] 4.4 Final report in the CLAUDE.md lane shape; the user decides the
  merge.
