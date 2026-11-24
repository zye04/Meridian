# Chlorophyll parity programme: design

Status: active programme (2026-11-24 review): its pillars and gaps are tracked on the GitHub board and in `2026-11-21-look-gap-audit.md`. Inputs:
`2026-11-21-look-gap-audit.md` (gaps G1-G11 with evidence),
`2026-11-21-chlorophyll-baseline.md` (graded facts, frame inferences,
Meridian capabilities, cross-check), research reports
`docs/superpowers/research/2026-11-21-{A..E}-*.md`, matched views
`docs/superpowers/research/matched-views/`.

## Intent

An engine that reaches, then surpasses, the Chlorophyll Engine's raster
photorealism: fine detail that survives up close, at distance and in
motion; correct, high-quality lighting; materials that react to their
environment like the real thing. In the user's words: a 4M-polygon tree
"has to look almost like an actual photorealistic picture", without "the
smeared, blurry" look of temporally reconstructed techniques (path
tracing, frame generation, heavy upscaling), which hide fine detail.
No single reference shot comes first: the user watched about an hour of
his showcase and wants that fidelity everywhere.

## Goals

- Motion clarity: no smear, no shimmer, detail holds while moving.
- Fine detail preserved at every distance (1 m to the horizon).
- Lighting quality and correctness (occlusion, translucency, shadows,
  exposure).
- Materials that hold up like a photo up close and far away.
- Performance as the proof of right technique: >= 180 fps average
  (5.5 ms) at native 2560x1440 on the RTX 4080, >= ~105 fps (9.5 ms) in
  the heaviest views (his ~130 fps average / ~80 fps dense forest on an
  RTX 3080, scaled by the 4080's ~30% lead).

## Non-goals (this programme)

- Content authoring (terrain shape, terrain material, placement art):
  phase 2, Houdini. Gaps that are content (G8) are tracked, not solved
  here, except engine hooks the content will need (e.g. colour maps).
- Ray/path tracing on the default path (stays an A/B only).
- Upscalers and frame generation as the way to reach the bar.

## Anti-goals (failure even if it "works")

- Anything that looks right only when still.
- Reaching 180 fps by lowering fidelity, density or detail.
- A look that depends on heavy temporal accumulation to hide noise.

## Design rules

1. **Each frame correct on its own.** Prefer deterministic techniques
   that give a stable image in one frame; temporal accumulation is light
   polish, never what makes the image work. Stability comes from the
   inputs (filtered coverage, coverage-correct mips, specular AA, no
   sub-pixel geometry flicker), with TAA kept light.
2. **Cost follows visible pixels, not content.** A 4M-triangle tree
   300 m away costs its ~2000 pixels. Wherever cost scales with triangle
   count, instance count or overdraw, the technique is wrong; fix the
   technique, never cut the content. Heavy, detailed assets are allowed
   and are themselves a test.
3. **Chlorophyll's approach by default**, unless something is known to be
   better (then logged in decisions.md with the reason).
4. **Evidence for every claim:** matched-view side-by-sides with his
   frames, motion gates, the benchmark route; research cited and verified.

## Approaches considered

- A. Biggest visible wins first (GTAO, foliage, voxels, shadows), perf and
  AA after: fastest visible change, but judged through known distortions
  (broken materials, undiagnosed grey image, unmatched views) and growing
  perf debt.
- B. Foundations first (tools, correctness, the perf architecture incl. a
  visibility buffer) before any look work: trustworthy, but days without
  visible change and the riskiest item first.
- **C. (chosen by the user)** A short phase 0 of the cheap things that
  distort every judgment, then five pillars run in parallel (two or three
  at a time), Materials and Lighting first.

## Structure

### Phase 0: trustworthy judgment (short)

- G6 tree material import fixed (metallic leak, layer blends, normals)
  - in flight, lane `ue-trees`.
- G7 grey/washed image diagnosed (exposure in forests, haze, GI, saved
  grade file) and fixed if engine-side.
- Matched views for the five reference frames (in flight).
- A fixed benchmark route (fly-through with `--benchmark`-style
  recording: average fps, 1% lows, per-pass ms) through meadow, forest
  edge, forest interior and aerial; the 180 / 105 fps lines; a per-pass
  ledger used as a diagnostic of what doesn't scale with pixels.
- A small resemblance metric per matched view (colour distribution of
  sky / foliage / grass / shadows, shadow-to-sunlit ratio, contrast, edge
  sharpness), as a trend, the user's eye stays the judge.

### Pillar 1: Materials and texture fidelity (first)

- An import standard for every asset (Megascans Trees, plants, surfaces):
  metallic 0 for organics, translucency/thickness and AO maps routed,
  Megascans layer blends reproduced (vertex-colour driven trunk / tileable
  bark / decoration), normal convention checked, all documented in
  docs/assets.md.
- Close-up fidelity: texture resolution where it is seen (8K scans vs our
  2-4K cooks), detail textures and/or texture streaming (virtual
  texturing) when budgets demand it; anisotropic filtering everywhere.
- Distance fidelity: coverage-preserving alpha mips (partly done), normal
  map filtering into roughness (Toksvig/LEAN-class), correct albedo and
  normals feeding the voxel bake.
- Judge: a material test bench per asset type (bark, leaf, grass, rock,
  ground) at 1 m / 10 m / 100 m / 1 km beside the source scan.

### Pillar 2: Lighting (first)

- G2: GTAO (XeGTAO-class) with bent normals, multi-bounce fit and
  specular occlusion, full resolution with a spatial denoise (rule 1);
  normals written always; applied to ambient only; reconcile with micro
  occlusion and crown visibility (no double darkening). Research B.
- G3/G4: a two-sided thin-leaf foliage shading model (tinted transmission
  scaled by the translucency map, forward-scatter lobe, wrap), grass
  additions (root-to-tip gradient, curved normals, per-clump variation,
  colour matched to the terrain). Research C.
- GI (`--gi probes`): finish only where it serves these (forest floor,
  canopy occlusion), default stays off until it passes the gates.

### Pillar 3: Far tier

- G1: filtered, continuous voxel coverage (no binary hash), crown-scale
  normals/density gradient and occlusion, correct voxel albedo; a larger
  voxel switch if it holds detail (measured). Research A.
- G11: far tree shadows (the WIP branch, then an optical-depth far map;
  Horizon's height-field far cascade pattern). Research A/D.
- His architecture: voxels generated in the LOD build (cook time),
  instance-less tile aggregates far away (memory at 10M trees) (F3, F4,
  F22). Also stated: distant grass takes the same voxel path (F23) and
  the per-asset voxel opacity volume gives each crown's light optical
  depth plus inter-leaf scattering (F21); backlog rows.

### Pillar 4: Shadows

- G5: Bend Studio screen-space shadows replacing the contact march;
  grass and small ground cover out of the shadow maps, contact shadows
  alone, as CE does (F26; backlog row, A/B);
  a sharper cascade 1 (4096^2 or a tighter split) with scrolling;
  contact hardening by marching the cascades (UE5 SMRT-like) rather than
  PCSS; hashed alpha in the shadow pass. Research D.

### Pillar 5: Clarity and performance

- G9: input-side stability (above) plus TAA refinements (reactive /
  thin-feature history control, luminance weighting); FSR 3.1 native AA
  as an A/B reference only. Research E. CE's AA method is unknown; he
  follows DLSS/FSR guidance and adds a lower mip bias on alpha-tested
  grass and a motion-based smear mask that sharpens selectively, and
  reactive masks did not help him (F16): candidates to A/B.
- G10 and rule 2: the visibility buffer for sub-pixel geometry shading,
  cheaper voxel brick listing; load-time work (cook, bake, transcode)
  threaded and cached.
- G7 follow-ups: exposure metering per environment, tonemapper A/B (GT7,
  Tony McMapface vs AgX) through the existing LUT. Research E.

## Tracking on GitHub (repo is public)

- One issue per gap (label `gap`, plus pillar label and
  `engine` / `content` / `pipeline`), body: baseline evidence (our
  captures only, never his frames; link his video), what we think he did
  with confidence, research links, acceptance gate (numbers vs the
  matched view, ms), links to spec/plan. Sub-issues = workable pieces,
  closed by commits.
- One milestone per phase/pillar; a Projects board: Acknowledged ->
  Investigating -> Planned -> Implementing -> Verified.
- Created 2026-11-21: issues #1-#12 = G1-G12 (same numbers), #13 matched
  views, #14 benchmark route + ledger, #15 resemblance metric; labels
  `gap`, `phase-0`, `pillar:*`, `engine` / `content` / `pipeline` /
  `tooling`; one milestone per phase and pillar.
- The repo docs stay the source of truth (audit, decisions.md, specs,
  plans); issues track and link, nothing is written twice. The audit's
  G-numbers are the issue keys.

## Constraints

- Usage: weekly limit ~93% used on 2026-11-21. Phase 0 items in flight
  continue; new implementation lanes wait for the reset. Research and
  tracking setup are cheap and can proceed.
- One GPU: lanes share the flock; two or three pillars at a time.
- Shared shader files (`lighting.glsl`, `mesh.frag`, `common.glsl`):
  small edits, frequent rebases (existing lane rules).
- Everything stays parametric and engine-side; no tuning to the Napf.

## Calls made (answers to the builder check, 2026-11-21)

Marked as the orchestrator's calls unless stated as the user's.

1. **The fps bar is proof, not a merge gate** (user: performance is "a by
   product of proper technique"). There is no programme exit date: it is
   ongoing toward parity, then beyond. An item may land over the ledger
   temporarily if its technique is right; a view that misses the bar
   opens the question "what doesn't scale with pixels", answered in
   Pillar 5. Sign-off per item: the gates (numbers) plus the user's eye.
2. **Baseline today** (Release, 2560x1440, native TAA, raster shadows,
   2026-11-21): start view 6.6 ms, walk-height grass 6.9 ms, forest edge
   with `--trees` 13.2 ms (scene early 8.3, voxel brick listing 1.5,
   voxel trees 0.9, clouds 0.75), forest interior 6.8 ms, forest
   overview ~13 ms. Phase 0's benchmark route replaces these with route
   averages and 1% lows.
3. **The visibility buffer is conditional, with a trigger** (CE is a
   visibility-buffer deferred renderer with all shading in compute and
   names it his foliage-overdraw answer, F17; rule 3 would make it the
   default, this call keeps it gated until the user revisits it): after
   Pillar 3's smooth voxel switch lands, if the forest edge still exceeds
   9.5 ms and shading of sub-pixel triangles is over ~1.5 ms of it, the
   visibility buffer starts (its own spec). To keep that cheap, shading
   math stays in shared functions (`lighting.glsl`, material evaluation
   in callable form), so moving from forward mesh shading to a
   visibility-buffer resolve reuses it instead of rewriting it.
4. **Evidence grade decides firmness:** items resting on his stated
   approach (S) are the default; items resting on inference (I) or press
   (P) are hypotheses: a short spike or A/B before commitment (e.g.
   cook-time voxel generation, instance-less aggregates).
5. **Dependencies and safe pairs:** Pillar 1's import standard precedes
   Pillar 3's voxel albedo; GTAO lands before the foliage model (the
   model is tuned under correct occlusion); Pillar 4's shadows land after
   GTAO (to judge contact vs ambient darkening together). Safe in
   parallel: Materials with Lighting (different files after G6), Far
   tier with Shadows (except the far shadow map, owned by Far tier),
   Clarity (TAA inputs) with any. Not in parallel: two items editing
   `lighting.glsl`'s ambient path at once.
6. **Schedule:** no deadline. Until the weekly reset only phase 0 items in
   flight, research and tracking. The MAX plan runs to ~2026-12-19; after
   that the programme continues at Pro limits, so pillars are staged one
   or two at a time and each item stays small enough for one lane.
7. **Motion reference:** his footage is compressed video, so it is a
   qualitative reference only (downloaded once for frame-stepping, kept
   out of the repo); the numeric motion measure stays our gates
   (Laplacian, shimmer, SSIM, flicker), re-anchored to the native TAA
   baseline of 2026-11-21.

## Left to the builder

- Per-item specs and plans (each pillar item gets its own spec when it
  starts, from the research report).
- Exact gates' numbers once matched views and the benchmark route exist.
- Lane assignment and parallelism within the constraints.
