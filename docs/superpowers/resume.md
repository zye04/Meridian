# Where work stopped (2026-11-21 evening, orchestrator handoff)

Weekly usage was exhausted (~96%); it resets **Tuesday 2026-11-24
05:00**. No lanes are running. Start the next session from the programme:

1. `docs/superpowers/specs/2026-11-21-chlorophyll-parity-programme-design.md`
   (phase 0 + five pillars, rules: each frame correct on its own; cost
   follows visible pixels; 180 fps native 1440p on the 4080 as proof).
2. GitHub board https://github.com/users/zye04/projects/1 (issues #1-#12 =
   audit gaps G1-G12; #13 matched views, #14 route, #15 metric).
3. `docs/superpowers/specs/2026-11-21-look-gap-audit.md` (evidence),
   `2026-11-21-chlorophyll-baseline.md` (graded facts), research
   `docs/superpowers/research/2026-11-21-{A..E}-*.md`.

## Ready to run after the reset (in this order)

1. Re-run the parity route on the Megascans Trees and the 70 deg FOV:
   `--benchmark 60 out.csv --route tools/routes/parity.route --trees`,
   `python3 -I tools/bench-route.py` (baseline in decisions.md predates
   both; 1% lows of ~20-26 fps = hitches to find, #14 comment).
2. Phase 0 Task 4: G7 grey-image diagnosis (#7), plan
   `docs/superpowers/plans/2026-11-21-phase0-trustworthy-judgment.md`.
3. GTAO (#2): spec `2026-11-21-gtao-design.md` (draft, user to confirm 3
   decisions) -> plan -> implement (Sonnet lanes).
4. Materials (#12, #6): spec `2026-11-21-materials-fidelity-design.md`
   (draft) -> plan; first the specular A/B for the silvery sheen.
5. Matched views refined at the new FOV (#13); motion gates re-anchored at
   70 deg (they were anchored at ~101 deg).

## Landed 2026-11-21 (afternoon)

Megascans Trees from Unreal (alder, beech, hornbeam, every variant;
`--trees`; forest edge 16.3 ms, #10), benchmark route + ledger, resemblance
metric (shadow/sunlit ratio 0.74 vs his 0.41: ambient too strong in
shadows -> GTAO), configurable FOV (`--fov`, default 70 deg horizontal,
was ~101; start view 7.9 ms at 70 vs 6.7 at 101), matched views (rough).

## Unreal setup

UE 5.9.0 preview-2 at `~/Apps/UnrealEngine` + Linux Fab and Bridge
plugins (`Engine/Plugins/`); project `~/Documents/Unreal Projects/MyProject`
(Python, EditorScriptingUtilities, GLTFExporter enabled); packs: Black
Alder, European Beech, European Hornbeam. Export/cook steps:
docs/assets.md "Megascans Trees from Unreal".

## Decisions waiting for the user

1. GTAO spec's 3 decisions; materials spec's 3 decisions.
2. GI default (off until forest cost and motion gates pass).
3. Saved grade file overrides the starting grade (G panel Reset + Save).

---

# Previous handoff (2026-11-20 ~23:55)

Read this first when resuming. Durable state is in git; the SDD ledgers
and each lane's `next.md` live under git-ignored `.superpowers/` in the
lane's worktree beside the repo (not on the remote). A new orchestrator
session takes over from here. Every lane below was stopped cleanly by the
user's request: branches pushed, `next.md` written, no runs left.

## Direction set by the user on 2026-11-20

- **Raster first, in the Chlorophyll Engine style.** Ray Reconstruction is
  no longer the default (DLSS Super Resolution DLAA is; `--rr on` is the
  A/B). Raster sun shadows are to replace the TLAS shadow ray. Ray
  tracing, RR and the 2.5D cloud path stay in the engine behind flags; none
  is deleted.
- **Clouds: keep the volumetric look, make it sharp and shimmer-free.** The
  user likes today's volumetric look and wants a crisper silhouette
  ("almost a pseudo outline"), also on the inner lobes (a strength in the G
  panel's Effects section, 0 = off), and no shimmer. The 2.5D slab was
  tried and paused (look rejected; it was 2.5-4x calmer, which is the
  stability bar).
- **Trees: Quixel Megaplants exported from Unreal Engine with textures.**
  SpeedTree and procedural generators are rejected. The user is installing
  UE 5.x on Linux (Linux build, unzipped to `~/Apps/UnrealEngine`).
- **Phases.** Phase 1: every engine capability works reasonably (stable
  clouds, raster shadows, GI, far field, trees, performance). Phase 2:
  build the environment (terrain, materials, placement) in an authoring
  tool; Houdini is the lead candidate (the Chlorophyll author does "all
  terrain and scattering work in Houdini, rule based"; Linux-native,
  scriptable headless via hython). Grass, terrain shape, terrain textures
  and texture placement look bad to the user; those are phase 2 unless
  they turn out to be engine bugs.
- **Chlorophyll Engine architecture** (stated by its author): CLODs
  (virtual geometry) with voxels for far representations, no impostors;
  hybrid screen-space + voxel GI, no ray tracing; proxies without instance
  data for very far away; 2.5D art-directed clouds; weather systems built
  in from the ground up.

## The user's complaints, in their order (2026-11-20 evening)

1. Volumetric cloud shimmer still there. Lane `clouds-sharp` (below).
2. 180 fps looking at the sky, ~70 fps in walk mode near grass. Not
   streaming (`stream` is 0.00 ms); it is the cost of drawing the grass.
   No lane yet: a meadow performance lane (measure overdraw, the shadow
   cascades' grass share, the distance thinning; then fix) is the
   recommended next dispatch. Note: the alpha-mip fix in 19ac9d1 means
   the grass/flower plants cooked before it lose their leaves beyond ~5 m
   until they are re-cooked; check that first.
3-6. Grass look, terrain shape, terrain textures, texture placement:
   phase 2.

## Recommended order when resuming (ask the user first)

1. `clouds-sharp` (the user's top complaint).
2. Meadow performance lane (new).
3. Raster shadows: tune, then flip the default.
4. GI: Task 5 merge, grass cost, forest fix, then the screen pass.
5. Far field: the mid tier rasterized (user approved), then far-c.
6. Trees: as soon as the user sends the Unreal inventory output.

## How work runs: one orchestrator

One orchestrator session is the user's only interface. It does no
implementing, reviewing or research itself: it dispatches one
sub-orchestrator per lane (background agent, self-contained brief) and
relays reports and user decisions. Each sub-orchestrator runs SDD
(Sonnet implementers, Sonnet reviewers, Opus only for look judgment and
risky reviews; never Fable) in its own worktree beside the repo (never
under `/tmp`), and merges to main by rebase + fast-forward after review.

The user's rules:

- Time: work fast and right. The user sets hard deadlines (2026-11-20:
  "00:00 MAX") and may ask for more agents: lanes then cut scope to a
  first visible version behind a flag, run several Sonnet implementers on
  non-colliding files at once, and say within 15 minutes what can land.
  Every milestone message carries an absolute local-time ETA; no padding.
- Parallelism: run queued work in parallel when files don't collide. The
  GPU flock is the real bottleneck with 4+ lanes. Watch the 5-hour usage
  window (the user reports %); near the limit, pause lanes with a WIP
  commit + push + a `next.md`.
- Don't launch lanes the user didn't ask for; when the user asks for help
  choosing, answer directly. When the user reports a visual problem,
  dispatch a fix, not a lane that proves it, and verify it in the user's
  conditions (clouds drifting at `--time-scale 1`, mouse-like turns,
  interactive settings), not only static deterministic captures.
- Prove a look claim with a side-by-side capture before relaying it.
- Look target: the Chlorophyll Engine frames in
  `assets/reference/chlorophyll/` plus `assets/reference/target-meadow.png`,
  except the clouds (keep today's volumetric look).
- Ask the user before deciding look questions; give a recommendation.

Every lane brief carries: no AI attribution in commits; latest SDK
versions (verify at fetch time); tidiness (no debug leftovers, new CLI
flags in CLAUDE.md, plan Status lines, docs state current truth only);
commit messages and temp files inside the lane's worktree (never the
shared session scratchpad); every game run through `flock
/tmp/meridian-gpu.lock`; never `pkill`/`killall` or any name-pattern kill:
record the PID of every background process at launch and stop only those
(a lane's `pkill -f 'sleep 900'` on 2026-11-20 may have hit another
lane's timer); look at assets before trusting their names; captures at
2560x1440 and GPU ms compared only with identical `Render WxH` lines;
DLSS SDK reuse via `-DFETCHCONTENT_SOURCE_DIR_DLSS_SDK=
/home/zye/Projects/dev/meridian-motion-dlss/build/_deps/dlss_sdk-src`;
every worktree's `assets/` is a symlink to
`/home/zye/Projects/dev/meridian/assets` (the far-field worktrees pointed
at `meridian-forests/assets` until 2026-11-20; check the link and the
placement hash in CLAUDE.md before measuring); new `.glsl` includes need
a cmake re-configure before shaders rebuild; ccache + mold are automatic.

Reports: "status" = running and waiting work only; "overview" = done.

## Landed on main 2026-11-20 evening (see decisions.md for each)

- Low-sun cost: terrain sun march once per small masked cluster
  (fc11e2a): scene early at sun 0.6 deg 61.7 -> 9.9 ms. The remaining
  ~10 ms near 0 deg is a backlog row (terrain shadow-height map).
- Clouds: per-frame texel jitter (5e5dd72), filter overshoot clamp, which
  fixed the blue fringes (64d21a6), sun disk occluded at the march
  early-out (b901b5d), stripe and horizon-band diagnoses as backlog rows
  (598ae29).
- DLSS SR DLAA is the default; RR is an A/B (27d03cb).
- Far field: compact bricks (1efb4cb) and a far sun shadow map (d599dcd).
  With `--trees`: forest overview 26.2 -> 18.1 ms, high overview
  36.9 -> 15.3; far field 394 MiB resident.
- GI first version, `--gi probes`, default off (79dddf2; 8900c3a,
  a8ace1a).
- Raster sun shadows, opt-in `--shadows raster` (4676c84).
- Textured tree cook, usd2gltf textures, the Unreal export script (draft)
  (19ac9d1, 5ec4f32, 1fc8cf5). 19ac9d1 also fixes the shared alpha-mip
  code (cutout leaves vanished beyond ~5 m).

## Lanes (all stopped; resume each from its next.md)

- **clouds-sharp** (`../meridian-cloudsharp`, branch `clouds-sharp`
  1fa9689, nothing merged). Baseline flicker under SR, mean / p99 / p99.9
  per 255: ground 0.137 / 1.45 / 3.38, in-layer 0.134 / 1.04 / 2.32
  (target p99 <= 0.6). 1fa9689 = a hashed per-texel start + golden-ratio
  step jitter, not measured. Suspected causes (unproven): ground: a 10%
  history blend with a large per-frame swing at the edges and a loose 2
  sigma clip; in-layer: reprojection by each frame's noisy cloud depth.
  Then the outer + inner pseudo outline with a G-panel strength. ~3-4 h.
  Flicker tool: `tools/cloud-flicker.py` on the branch (`--shift`
  unvalidated).
- **raster** (`../meridian-raster`, branch = main, all merged). 5 cached,
  texel-snapped 2048^2 cascades through the cluster cull, 3x3 PCF.
  Raster vs rt: forest with `--trees` ~7 ms faster, meadow 0.15-1.5 ms
  slower, crown shimmer equal, the beech forward sharpness 0.198 (gate
  0.20) vs rt 0.214. Left (~3-4 h): per-cascade re-render cost, 4-tap
  PCF + bias, rerun the gates, flip the default; streamed grass casts no
  cascade shadows; outer cascades draw trees at full detail (use the far
  field); the caster height is hard-coded for the Napf. Removing
  TLAS/BLAS waits until raster is the default (still used by `--shadows
  rt`, `farfield_bake.comp`, `--far-shadows tlas`).
- **gi** (`../meridian-gi`, branch `gi` c906abd WIP, not merged): Task 5
  (ground radiance cache, albedo feedback, multi-bounce, history 0.95);
  Release clean, Debug/review/decisions entry not done. Open: +1.7 ms
  scene early on grass with GI on (fix: sample the probes once per masked
  cluster); `--trees` forest interior goes blue and the probes cost ~3 ms
  (fix: canopy occlusion from the far-field column map); the effect is
  subtle. Then the hybrid screen pass, motion gates, look review. ~6 h in
  total.
- **far field** (plan `docs/superpowers/plans/2026-11-19-forests.md`,
  Status line current). `far-m` (`../meridian-farM`, 71b3ee9): the USER
  APPROVED option 1, rasterize the voxels the Nanite Foliage way (bricks
  as small cubes into the G-buffer, dithered coverage, exact depth and
  motion, per-brick occlusion culling), ~1 day; resume steps in its
  next.md. `far-c` (`../meridian-farC`, e753156, rebased): slower than
  main; next, test its culling with the old 6 px switch, keep only what
  pays (~2 h). Then Task 12; Task 13 (forest-edge variants) waits for the
  Unreal trees.
- **tree-import** (`../meridian-treeimport`, merged; branch = main
  1fc8cf5). Waiting on the user: Unreal steps in `docs/assets.md`
  ("Textured Megaplants trees from Unreal"): enable the Procedural
  Vegetation Editor plugin and "Nanite Foliage (Experimental)", add the
  beech from the Fab window, generate a variant, then run in the Output
  Log (Python): `py /home/zye/Projects/dev/meridian/tools/unreal/export_trees.py
  --folder /Game/<pack folder> --species beech --inventory-only`, then
  without `--inventory-only`; the user sends back `inventory.json`,
  `manifest.json`, `export_log.txt` from `~/Downloads/unreal-export/beech/`.
  Then 1-2 h (single mesh) or +1-2 h (branch assembly converter). The
  Megaplants USD zips on Fab carry no textures; the hornbeam page serves
  the oak zip.
- **clouds-25d** (PAUSED, not merged; `../meridian-clouds25d` 7839579
  with the `--clouds 2.5d|volumetric|off` switch 0a74d05, slab shader,
  spec, plan; `../meridian-clouds25d-mip` 29be63f, max-mip traversal,
  Release only). Resume only if the user asks.
- **sunset-fix** (DONE, all merged). Worktree `../meridian-sunset-fix`
  can be removed.
- **M3 meadow far-ground match** (PAUSED since 2026-11-20 ~18:40, branch
  `meadow-y`, `../meridian-meadow-y` WIP 1fc4abf); resume from
  `../meridian-meadow-stream/.superpowers/sdd/stream/next.md`. Obsolete
  branches: `meadow`, `meadow-accents`, `meadow-x`, `meadow-tint`,
  `meadow-cache`.
- **Motion clarity** (plan `docs/superpowers/plans/2026-11-20-motion-clarity.md`).
  RR is no longer the default, so Tasks 3.2a-3.2b (shadow denoiser for the
  fallback paths) depend on whether raster shadows replace RT: revisit
  after the raster default flips. Ledger
  `../meridian-motion/.superpowers/sdd/2026-11-20-motion-clarity/`.

## Queue (each when the user says go)

1. Meadow performance lane (see complaint 2).
2. Phase 2 environment authoring (Houdini; check the license for game
   export: Indie is the safe choice).
3. Weather beyond clouds: rain + wet surfaces, fog, wind on vegetation,
   snow (the weather map already carries precipitation).
4. Terrain detail (near displacement from the material height maps).
5. Frame pacing (backlog row).
6. Platform: the user can't run anti-cheat games on Arch; the
   recommendation was dual-boot Windows (games, Unreal, HDR testing)
   while the engine stays on Linux until there is a concrete reason to
   port it.

## Decisions waiting for the user

1. Raster shadows as the default: recommended after the tuning passes
   the shimmer/sharpness gates and the meadow cost is no worse than rt.
2. GI: the three recommendations in the gi lane (grass cost per cluster,
   canopy occlusion before look tuning, hybrid pass last and only if the
   motion gates hold).
3. The default grade: the user tunes it in the G panel and saves; read
   `~/.config/meridian/grade.json` before changing `kStartingGrade`.

## Done on main earlier

M1 (15 km Napf terrain); M2a Tasks 1-7 (static TLAS, Megaplants import,
placement rules, GPU placement, placed trees and rocks, cluster-group BVH
traversal); the quality pass; the Megascans ground material, POM, micro
and contact shadows, bloom, DLSS, exposure + grade, the G and F3 overlays,
streamed meadow placement, the dither pattern cache. Backlog highlights:
HDR output (mandatory), violet distant haze, procedural rivers/paths,
character animation.
