# Where work stopped (2026-11-20 ~19:00, orchestrator handoff)

Read this first when resuming. Durable state is in git; the SDD ledgers
under `.superpowers/` are local and git-ignored. A new orchestrator
session takes over from here.

## TOP PRIORITY for the next session: sunset GPU cost + cloud shimmer

The user's first ask when they're back. Reported from interactive play on
main a4e7623 (Release, 1440p fullscreen, DLSS RR DLAA, no `--trees`), on a
grassy ridge looking into a low sun (sun set from the G panel):

- **15 FPS, GPU 64.21 ms, of which `scene early` 56.89 ms.** At midday
  the open view's scene early is ~6.0 ms. Everything else was normal: aa
  (RR) 5.13, clouds 1.27, weather 0.32, far field 0, stream 0.
  Lead hypothesis (unproven): per-fragment sun shadowing on the dense
  alpha-masked meadow grass; at grazing sun angles the `terrainOccludes`
  height-pyramid march (terrain.glsl, called from mesh.frag) gets very
  long, and grass overdraw pays it many times per pixel. Other suspects:
  masked overdraw, the TLAS shadow ray, plant LOD.
- **Clouds and fine detail shimmer heavily**, clouds blocky at the
  edges, **blue dashed outlines along some cloud undersides**. RR is on.
  Part of it is likely the 15 fps (fewer frames for RR to accumulate);
  re-judge after the perf fix, then check the clouds' path through RR
  (guides, motion vectors, transparency mask, white noise), the cloud
  upsample/composite, and A/B `--aa taa` / `--rr off`.
- A fix lane started and was paused before any run: nothing measured or
  fixed. Worktree `../meridian-sunset-fix` (branch `sunset-fix`, pushed,
  identical to a4e7623; `assets/` linked to main's; Release build done in
  `build-release/`, no Debug build; capture helper
  `.superpowers/sf/cap.sh`). Its `.superpowers/next.md` has the suspects,
  repro commands, bisect steps (march off, ray off, sun elevation sweep,
  effects toggles, a heat map of march steps) and fix ideas. From code
  reading: `terrainOccludes` can take up to 256 steps per ray, paid by
  every grass fragment on lit and backlit faces; the TLAS ray is a weaker
  suspect (streamed grass isn't in the TLAS). Cloud code to look at:
  `clouds_composite.comp`, `clouds_resolve.comp`.
- Repro start: `--camera 1119 1356 -431 1.57 -0.02 --sun 0.9 0.05 -0.4`
  and the sunset view `1119 1500 -431 3.14 -0.15 --sun 0.9 0.05 -0.4`;
  sweep sun elevation 0.05 -> 0.5 and read `GPU passes:`.
- Target: scene early at low sun on the order of midday (<= ~7 ms at that
  view) with grass still receiving terrain/tree shadows; clouds stable.
  Dispatch it as a fix (root cause first, Opus diagnosis, Sonnet
  implementation), merge each fix as soon as it is verified.

## How work runs: one orchestrator

One orchestrator session is the user's only interface. It does no
implementing, reviewing or research itself: it dispatches one
sub-orchestrator per lane (background agent, self-contained brief) and
relays reports and user decisions. Each sub-orchestrator runs SDD
(Sonnet implementers, Sonnet reviewers, Opus only for look judgment and
risky reviews; never Fable) in its own worktree beside the repo (never
under `/tmp`), and merges to main by rebase + fast-forward after review.

The user's rules (2026-11-20):

- Time: no hard caps (the user dropped the 2 h box), but work fast and
  right: one implementer per chunk (or the sub-orchestrator implements
  small things itself), one Opus review of risky core code + one final
  review, iterate in Release (`--placement off` where possible), Debug +
  validation once before each merge, merge small working pieces early,
  batch captures, and every milestone message carries an ETA. Finishing
  passes ~20-30 min. Give lanes absolute local-time deadlines matched to
  the work; the user reacts badly to padded ETAs.
- Parallelism: run queued work in parallel when files don't collide.
  Watch the 5-hour usage window (the user reports %); near the limit,
  pause lanes with a WIP commit + push + a `next.md`, resume after reset.
- Don't launch lanes the user didn't ask for; when the user asks for help
  choosing, answer directly. When the user reports a visual problem,
  dispatch a fix, not a lane that proves it.
- Look target: the Chlorophyll Engine frames in
  `assets/reference/chlorophyll/` plus `assets/reference/target-meadow.png`
  (primary for meadow density and tree colour), not fidelity to the Napf.
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
/home/zye/Projects/dev/meridian-motion-dlss/build/_deps/dlss_sdk-src`;
every worktree's `assets/` is a symlink to main's (a partial asset set
changes the placement hash and every timing; check the hash in CLAUDE.md
before measuring); ccache + mold are automatic; GPU contention on the
flock is the main bottleneck, so batch runs.

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
  "Cloud shadow fallback refit", "Clouds and weather measured"; the
  SceneData offset fix 5790d88 came after, and the clouds were re-checked
  against it). User accepted for now: clouds ~2 ms (spec ~1 ms) and the
  sky's fast-turn sharpness under RR (0.878 vs 0.978); both revisit in
  the RR / frame-pacing pass. Worktrees removed; numbers in decisions.md.
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
  2. Trees: the user finds the Megaplants models themselves bad, not just
     untextured. Plan: SpeedTree Modeler 10.2.1 (installed at
     `~/Apps/SpeedTree_Modeler_v10.2.1`, trial; Indie $19/month if it
     passes). The user works in it with their own agent (beech first:
     mature/edge/lone/young variants, then oak, maple, edge shrubs; USD
     export with PNG maps to `~/Downloads/speedtree-export/<species>/`).
     When the user reports "beech test export ready", dispatch a lane to
     run it through usd2gltf + meridian-cook and capture it against
     `target-meadow.png`. Fallback idea if SpeedTree disappoints: a
     procedural tree generator scripted in Blender (space colonization +
     scanned Fab leaf/bark textures).
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
  `../meridian-forests/.superpowers/sdd/2026-11-19-forests/`). On main
  (2026-11-20): compact bricks + sliced splat (1efb4cb; far field 394 MiB
  resident, 1257 MiB load peak, gates pass) and the far sun shadow map
  (d599dcd; far field pass 2-3 ms at the overviews, was 7.5-11). Open,
  each with a `next.md` in its worktree's `.superpowers/`:
  - `far-c` (`../meridian-farC`, rebased on d599dcd): far-cell two-phase
    culling, column boxes, 1 px switch with hysteresis. Fails: far field
    6.0 ms high overview / 10.0 sunset vs 2.7 / 2.9 on main (more far
    cells from the nearer switch, larger boxes). Next: split the switch
    distance from the culling/box change (old 6 px switch with C's
    culling), keep only what pays.
  - `far-m` (`../meridian-farM`, 71b3ee9, not rebased): mid tier,
    per-instance box + DDA, 6.7-85 ms (gate 1.5). Waiting on the user:
    rasterize the voxels (Epic's Nanite Foliage approach, recommended) vs
    narrower band vs tuning; options in its next.md.
  Then Task 12 (measurement); Task 13 (forest-edge rules) waits for the
  SpeedTree models (the density/scale edge ramp exists; variants need the
  new prototypes). Gates are measured with `--rr off`. Worktree assets/
  must be main's (`ln -s .../meridian/assets`).
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

M2a far field (A, B, then fix C and the mid tier; then Tasks 12-13), M2b
(forest lighting). Held ideas: NRD SIGMA shadow denoiser + plain DLSS
DLAA instead of RR (~3 ms saved, likely sharper in motion; the user said
hold); a meadow cost pass (the meadow adds ~4 ms to scene early at
midday). Backlog highlights: HDR output (mandatory), violet distant haze,
procedural rivers/paths, character animation.
