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

- **G2 ground transitions + cliffs** (RUNNING, owned by the old
  orchestrator session until it merges; deadline 16:30). Branches
  `ground2` (worktree `../meridian-ground2`) and `ground2-cliffs`
  (`../meridian-ground2-cliffs`); next steps in each
  `.superpowers/sdd/ground2/next.md`. Don't touch them until G2 reports.
- **C2 clouds** (PAUSED at 15:25, branch `clouds` at 197d458, worktree
  `../meridian-clouds`; user chose: merge to main after Task 8, no look
  gate). Done: cumulus detail across the body, stepped underside fixed,
  size variety, altitudes (cumulus base 2.8 km, top 4.8, stratus
  1.7-2.8, cirrus 9), cost 1.92 ms fair. Next (ledger
  `docs/superpowers/wip-ledgers/clouds/progress.md`, CHECKPOINT 15:18):
  explain the darker low-sun shadow map (half-row update? heights? the
  new .pfm reader?), then refit the cloud-shadow fallback properly (user:
  no backlog row), motion numbers, Task 8, rebase, Opus final review,
  squash the WIP commits, merge. Sheets:
  `.superpowers/sdd/2026-11-19-clouds-weather/artifacts/t7/sharp/`.
- **M3 meadow** (PAUSED at 15:01). Branches `meadow` (worktree
  `../meridian-meadow`: step 1 7c248e0 terrain_roles.glsl + wind clock,
  also cherry-picked as 68ab406 in `../meridian-meadow-merge`, unpushed;
  grass WIP eef148d: mesh-shader blades wired, draws nothing yet) and
  `meadow-accents` (`../meridian-meadow-accents`, 1912f50: flower/clump
  placement layers 4-10; clover renders black, flowers too sparse,
  ~1.59M instances vs 1.5M budget). Resume order and details:
  `../meridian-meadow/.superpowers/sdd/meadow/next.md`, `grass-next.md`,
  and the accents `next.md`. Put the grass debugging on Opus. This is the
  biggest visible gap: the ground reads as a flat lawn.
  Overlap with G2: M3 step 1 moves the terrain role weights into
  `terrain_roles.glsl`, and G2 rewrites those weights (warped multi-scale
  noise). Resume M3 now on the grass shaders and accent tuning (no shared
  files); rebase step 1 onto G2's weights after G2 merges, and only then
  merge M3.
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

1. M2a Task 8: tune the voxel far field (C) or fall back to B (2.5D
   canopy); trees are hidden for now, so this waits.
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
