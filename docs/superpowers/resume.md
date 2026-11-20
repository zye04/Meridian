# Where work stopped (2026-11-20 ~15:40, orchestrator handoff)

Read this first when resuming. Durable state is in git; the SDD ledgers
under `.superpowers/` are local and git-ignored. A new orchestrator
session takes over from here; the old one finishes lane G2 only.

## How work runs: one orchestrator

One orchestrator session is the user's only interface. It does no
implementing, reviewing or research itself: it dispatches one
sub-orchestrator per lane (background agent, self-contained brief) and
relays reports and user decisions. Each sub-orchestrator runs SDD
(Sonnet implementers, Sonnet reviewers, Opus only for look judgment and
risky reviews; never Fable) in its own worktree beside the repo (never
under `/tmp`), and merges to main by rebase + fast-forward after review.

The user's rules (2026-11-20):

- Time: every lane gets an absolute local-time deadline, at most ~2 h,
  sized in hours, never days. Everything in a brief is finished, merged
  and pushed by the deadline, with no "backlog row instead of doing it";
  WIP is an emergency fallback only, and a lane at risk messages the
  orchestrator early.
- Parallelism: run queued work in parallel when files don't collide.
  Watch the 5-hour usage window (the user reports %); near the limit,
  pause lanes with a WIP commit + push + a `next.md`, resume after reset.
- Don't launch lanes the user didn't ask for; when the user asks for help
  choosing, answer directly. When the user reports a visual problem,
  dispatch a fix, not a lane that proves it.
- Look target: the four Chlorophyll Engine frames in
  `assets/reference/chlorophyll/`, not fidelity to the Napf (a test bed).
- Ask the user before deciding look questions; give a recommendation.

Every lane brief carries: no AI attribution in commits; latest SDK
versions (verify at fetch time); tidiness (no debug leftovers, new CLI
flags in CLAUDE.md, plan Status lines, docs state current truth only);
commit messages and temp files inside the lane's worktree (never the
shared session scratchpad); every game run through `flock
/tmp/meridian-gpu.lock`; never `pkill`/`killall` (stop only your own
PIDs); look at assets before trusting their names; captures at
2560x1440 and GPU ms compared only with identical `Render WxH` lines;
DLSS SDK reuse via `-DFETCHCONTENT_SOURCE_DIR_DLSS_SDK=
/home/zye/Projects/dev/meridian-motion-dlss/build/_deps/dlss_sdk-src`.

Reports: "status" = running and waiting work only; "overview" = done.

## Landed on main 2026-11-20 (see decisions.md for each)

Megascans ground material (14 surfaces, 4K colour/normal, height blends,
colour matching), rock anti-tiling, terrain POM + self-shadow, micro
shadows (terrain AO relative to layer mean), screen-space contact
shadows, sun glare (bloom), DLSS DLAA + Ray Reconstruction preset F as
default (absolute motion-sharpness gates, one fullscreen swapchain),
exposure + starting grade, leaf transmission, Poly Haven AO, trees hidden
by default (`--trees`), grass/flower plants cooked (coverage-preserving
alpha mips), ImGui overlay: G grading panel (grade file
`~/.config/meridian/grade.json`, Effects toggles), F3 performance
overlay, F4 / `--benchmark` CSV, kMaxPassTimes 32.

## Lanes: running, paused, queued

- **G2 ground transitions + cliffs** (DONE, merged 8a514ad and 05767d4;
  decisions.md "Natural ground transitions", "Non-repeating cliffs").
  M3 step 1 can now rebase onto G2's role weights. Worktrees
  `../meridian-ground2*` can be removed.
- **C2 clouds** (DONE, merged 2026-11-20; plan Status line, decisions.md
  "Cloud shadow fallback refit", "Clouds and weather measured"). Worktrees
  `../meridian-clouds*` can be removed; ledger and artifacts in
  `../meridian-clouds/.superpowers/sdd/2026-11-19-clouds-weather/`.
- **M3 meadow** (PAUSED 2026-11-20 ~18:40; most of it on main).
  Streamed placement (plan `docs/superpowers/plans/2026-11-20-streamed-placement.md`,
  decisions.md "Streamed placement", "Plant micro occlusion") is merged
  with the dense meadow, Prototype::tint (fresh green, cream hawkbit),
  wide thinning and the terrain-normal blend (40a230d), and the dither
  pattern cache (851d0ad). Paused and NOT merged: branch `meadow-y`
  (pushed; worktree `../meridian-meadow-y`, WIP 1fc4abf), the far-field
  match: terrain blade detail, distance gain, canopy sun shadow and
  `tools/placement-match.py`, all unverified. Resume from
  `../meridian-meadow-stream/.superpowers/sdd/stream/next.md`: re-measure
  the table, tune to 3-5% (low sun is the hard case), Debug validation,
  merge, motion gates at 30 and 150 m/s, then plan task 4 (TLAS for
  streamed instances). Obsolete branches: `meadow`, `meadow-accents`,
  `meadow-x`, `meadow-tint`, `meadow-cache`.
- **Queue after those** (in order, each when the user says go):
  1. Terrain detail: near-camera displacement from the material height
     maps (tessellated near nodes) + erosion-style procedural detail;
     cliffs via placed rock/cliff meshes. Maybe a more dramatic terrain
     (user's call; the Napf is gentle).
  2. Leaf and bark textures for the Megaplants trees (USD leaves have UVs
     into per-species atlas regions; remap onto a downloaded leaf
     texture; bark tiles along the branch). Waits on the user
     downloading a leaf + bark. Crown darkening (cook-baked AO) with it.
  3. Frame pacing (backlog row).
  4. RR motion sharpness pass (last, user order): inputs (motion vectors
     incl. clouds, masks, hit distance, mip bias), tighten gates.

## Decisions waiting for the user

1. (Resolved 2026-11-20: voxels, no impostors, three tiers; see the
   far-field spec. Look target for trees:
   `assets/reference/target-meadow.png`.)
2. The default grade: the user tunes it in the G panel and saves; read
   `~/.config/meridian/grade.json` before changing `kStartingGrade`.

## In progress (WIP branches on origin)

- **Motion clarity** (plan `docs/superpowers/plans/2026-11-20-motion-clarity.md`,
  spec `docs/superpowers/specs/2026-11-20-motion-clarity-design.md`; its
  Status line has the next steps). On main: gate tools, grading (CPU
  reference + GPU LUT bake), render/display split, transparency mask,
  Upscaler interface, TAA v2 (forward 5 m/s 0.94, 0.17 ms at 1440p).
  DLSS module (Task 1.3), guide buffers and the `whiteNoise` field (Task
  1.4) and Ray Reconstruction (Task 1.5) are on main. Default since
  2026-11-20: DLSS DLAA + RR preset F (4.71 ms at 1440p; fallbacks SR
  DLAA preset M, then TAA v2); the Laplacian gates are absolute motion
  sharpness (decisions.md, "DLSS DLAA with Ray Reconstruction is the
  default"); fullscreen startup builds one swapchain. Next: 3.2a -> 3.2b
  (shadow denoiser, fallback paths only), Task 4 sweep, Task 5 final
  review.
  Ledger:
  `../meridian-motion/.superpowers/sdd/2026-11-20-motion-clarity/`.
- **M2a far field, three voxel tiers** (spec
  `docs/superpowers/specs/2026-11-20-far-field-design.md`, plan
  `docs/superpowers/plans/2026-11-19-forests.md` Tasks 9-13, ledger in
  `../meridian-forests/.superpowers/sdd/2026-11-19-forests/`). On main:
  the far-field prototype (cddcd66; `--trees` only; gates failing as
  prototyped). Paused 2026-11-20 on four pushed branches, each with a
  `next.md` in its worktree's `.superpowers/` (state, gates, resume):
  - `far-a` (`../meridian-farA`, e334b7c): compact bricks + sliced splat;
    load line 394 MiB resident / 1257 MiB peak (gates 500 / 2500). Left:
    re-measure on the real assets, one Debug run (3 validation errors
    from a missing TRANSFER_SRC flag, fixed, unverified), merge first.
  - `far-b` (`../meridian-farB`, 748520c): sun shadow map of the bricks,
    double-buffered, amortised rebuild; Opus review fixes in code, GPU-
    unverified. Left: gates + sunset check on the real assets, Debug.
  - `far-c` (`../meridian-farC`, c8b84a8): far-cell two-phase culling,
    column boxes, switch hysteresis. Far pass slower than main's far pass
    on its runs (per-pixel shadow march dominates; B's map is the lever).
  - `far-m` (`../meridian-farM`, 71b3ee9): mid tier, prototype voxels per
    instance (bake, switch, draw work). The voxel pass costs more than the
    clusters it replaces (forest overview 85 ms): needs a structural fix
    or a user decision on switch distance / voxel pixel size.
  Then plan Task 12 (measurement) and Task 13 (forest-edge placement
  rules: size gradient, open crowns outside, sparse understory).
  Worktree assets/ must be main's (`ln -s .../meridian/assets`): a partial
  asset set changes the placement hash and every timing.
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
