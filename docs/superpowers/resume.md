# Where work stopped (2026-11-20, usage limit)

Read this first when resuming. Durable state is in git; the SDD ledgers
under `.superpowers/` are local and git-ignored.

## How work runs: one orchestrator

One orchestrator session is the user's only interface. It does no
implementing, reviewing or research itself: it dispatches one
sub-orchestrator per lane (background agent, self-contained brief), and
relays reports and user decisions. Each sub-orchestrator runs
subagent-driven development (implementer, task reviewer, fixer, final
reviewer) in its own worktree beside the repo (never under `/tmp`, which
is wiped on reboot; artifacts under `.superpowers/sdd/<plan>/artifacts/`),
and merges to main by rebase + fast-forward after a clean review. Every
game run goes through `flock /tmp/meridian-gpu.lock`.

Usage budget (the 2026-11-20 window ran out with five lanes at once):

- At most two build lanes at a time; research is a short capped report.
- Implementers and docs agents on Sonnet; Opus only for risky reviews and
  judgment. Never Fable.
- Give the user an expected usage cost before fanning out.
- Prefer work with a visible result.
- At ~90% usage: every lane commits labeled WIP, pushes, records its next
  step here, and stops.

Every lane brief carries: no AI attribution in commits; the latest
version of any SDK (verify at fetch time); the pre-merge tidiness check
(no temporary debug code, flags or stray files; new CLI flags get a
CLAUDE.md line; the plan's Status line updated; deferred items as
backlog rows; docs state current truth only).

Reports to the user: "status" = running and waiting work only;
"overview" = what is done.

## Priority for the next window (user review, 2026-11-20)

The user played main in Release and judged it "absolutely terrible".
Screenshots: `.superpowers/user-review-2026-11-20/1-4.png` (local).
Decision: put the tree/far-field work (M2a Task 8+) and DLSS on hold
and do a visible-quality pass first. Every asset looks bad, not just
the trees, which points at a systemic pipeline cause rather than the
assets themselves. Order:

1. **Asset fidelity investigation** (first lane, diagnose before fixing).
   Render one tree, one rock and one Poly Haven model in a reference
   viewer (Blender / a glTF viewer, same HDRI-ish light) and in Meridian,
   side by side. Check the cook (texture compression, sRGB vs linear,
   normal-map convention, mip bias, roughness/AO channels, decimation:
   Megaplants 4M tris to what), the shading (sky ambient, the missing
   bounce light, near-black shadows, exposure), and the LOD (far trees
   render as leafless sticks while their RT shadows are full canopies:
   raster LOD vs full-detail BLAS). Report the root causes with evidence.
2. Green ground: the Napf is pasture, but everything renders as bare
   dirt; the terrain material, then M3 meadow grass.
3. Rocks: seated on slopes, no floating or tilted slabs.
4. Steep-slope texture stretching (triplanar or similar).
5. Shadow and foliage shimmer in motion: Ray Reconstruction (motion
   clarity plan) or the fallback denoiser.
6. Finish and merge clouds.

## Decisions waiting for the user

1. M2a Task 8 gates (see below): tune the voxel far field (C) with the
   spec's levers, or fall back to option B (2.5D canopy). Measure the
   shadow and coverage gates first.
2. Clouds: the motion-sharpness trade-off the clouds lane will write up,
   then the Task 7 comparisons before Task 8.

## In progress (WIP branches on origin)

- **Motion clarity** (plan `docs/superpowers/plans/2026-11-20-motion-clarity.md`,
  spec `docs/superpowers/specs/2026-11-20-motion-clarity-design.md`; its
  Status line has the next steps). On main: gate tools, grading (CPU
  reference + GPU LUT bake), render/display split, transparency mask,
  Upscaler interface, TAA v2 (forward 5 m/s 0.94, 0.17 ms at 1440p).
  DLSS module (Task 1.3) on main: TAA v2 stays the default, DLSS 4.5
  (DLAA preset M) is opt-in with `--aa dlss` (decided 2026-11-20, see
  decisions.md). Next: 1.4 -> 1.5, 3.2a -> 3.2b, docs sweep, final review.
  The clouds branch must follow the plan's clouds rebase checklist
  (cloud targets sized from `renderExtent`, transparency mask, white
  noise under Ray Reconstruction). Ledger:
  `../meridian-motion/.superpowers/sdd/2026-11-20-motion-clarity/`.
- **M2a Task 8** (`wip/m2a-task8` at 156e3fc, worktree
  `../meridian-forests`, ledger in its
  `.superpowers/sdd/2026-11-19-forests/`, numbers in `task-8d-report.md`):
  voxel far-field prototype, task-reviewed, builds. Gates at 1440p
  Release so far fail: VRAM resident 659 MiB / 500, load peak 3.68 GB /
  2.5; far pass 2.4 ms ridge, 6.1-8.5 ms overview / 1.0; voxel far
  shadows 1.5-2.4x the TLAS path. Shadow and coverage gates unmeasured
  (their helper scripts may be lost). Next: measure them, then stop for
  decision 1; afterwards docs, squash, merge or switch to B.
- **Clouds** (`clouds` at 2399906, worktree `../meridian-clouds`, plan
  `docs/superpowers/plans/2026-11-19-clouds-weather.md`). Tasks 1-6 done
  and reviewed. Task 7 (look development) fix round is WIP 2399906:
  unbuilt, uncaptured, unreviewed. Next steps are in the branch ledger
  `docs/superpowers/wip-ledgers/clouds/progress.md`: refit the cloud
  shadow fallback, write up the motion-sharpness trade-off for the user,
  regenerate the comparisons, regression checks, re-review, fold into
  782dd03, then stop for the user before Task 8. Comparisons made before
  the fixes: `../meridian-clouds/.superpowers/sdd/2026-11-19-clouds-weather/artifacts/t7/final/`.
  Before merging: copy the ledger back into `.superpowers/` and delete
  `docs/superpowers/wip-ledgers/`; then set the clouds plan's Status line.

## Done on main

- M1 (15 km Napf terrain).
- M2a Tasks 1-7: static TLAS, Megaplants import, placement rules, GPU
  placement (2.9M instances), placed trees and rocks drawn (cell cull,
  TLAS fill), cluster-group BVH and its GPU traversal (0 drops; ~11.5 ms
  of raster at the forest edge at 1080p until the far field lands).
  Geometry pools sized from the loaded content.
- Quality pass: sky dithering, summer haze, terrain march fix, multi-
  bounce AO + specular occlusion, per-layer terrain AO (shadowed ground
  ~11% of sunlit). `--capture x.pfm` + `tools/region-luminance.py`.
- Far-field spec (`docs/superpowers/specs/2026-11-20-far-field-design.md`).
- Repo hygiene: CLAUDE.md trimmed, docs map at its top, known gaps in
  `docs/backlog.md`, asset notes in `docs/assets.md`.

## Next after that

M2a Tasks 9 (far-field completion), 10-11 (mid-range impostors), 12
(gate: forest views <= 16.7 ms with shadows). M2b (forest lighting), M3
(meadow). Backlog highlights: HDR output (mandatory), violet distant
haze, procedural rivers/paths, character animation. The user's look
target is the Chlorophyll Engine video (frames in
`assets/reference/chlorophyll/`).
