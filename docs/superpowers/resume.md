# Resume here (entry point; updated 2026-11-26)

Read this file first, ask the open questions below, and update the file
before ending the session. It holds current truth only: open questions,
running lanes, the ordered next steps, the gates, and the state of main.
Session history lives in git (`git log -- docs/superpowers/resume.md`) and
in `docs/decisions.md`.

## Open questions for the user

At most three, each answered in chat (the user opens no documents); the
agent writes the answer into the spec or here.

None open.

Decided by the user 2026-11-24, not yet applied: reset the saved grade file
to the starting grade (the file `~/.config/meridian/grade.json` overrides
the starting grade in every interactive run). To apply, either delete the
file (`rm ~/.config/meridian/grade.json`; a missing file means the
defaults, and the sun, haze and fov it holds reset too), or open the game,
press `G`, click Reset, then Save. Whoever next runs the game
interactively does it; it was not done in the docs lane. Board issues #9
and #17 were already closed on 2026-11-24 with a reason comment each.

No spec waits for approval: the broadleaf forest floor spec is decided
and sequenced, the GTAO spec is superseded (rewrite after the visibility
buffer), the DAG voxel design is approved.

## Lanes running

| Branch | Window / worktree | State |
|---|---|---|
| `g1` | main checkout (`~/Projects/dev/meridian`) | merged into main 2026-11-24 (fd3df78); the checkout stays on `g1` until the next lane |
| `main` | `../meridian-main` | main's worktree; the g1 merge was built and verified here |
| `fix-light-temporal` | `../meridian-lane-b` | paused, `RESUME.md` in that worktree |
| `canopy-backlit` | `../meridian-canopy` | merged; kept for its 1024 spp references and scripts in `.superpowers/spike-backlit/` |
| `voxel-look` | `../meridian-voxel-look` | merged into main 2026-11-25; its notes, tools and captures in `.superpowers/voxel-look/` (RESUME.md) |
| `debug-tools-2` | `../meridian-debug` | round 1 merged 2026-11-26; round 2 on branch `debug-tools-2` (from main 4f01f72), not merged: views stretch/sun/ambient/specular/luminance, light split + stretch + EV in pixel.json, F8 solo/hide, F7 time freeze (cli.md "Debug views and the report"); first use diagnosed the user's alder bundle (decisions.md "Debug tools round 2", backlog "Alder bark wet gloss"); next: user review, merge; round 3 in backlog |

Lane report shape (every lane ends its report with these five, numbers
at 2560x1440): done, measured, look (image paths), needs from the user,
next. The user relays between windows by pasting them.

## Ordered next steps (Fable assessment, 2026-11-24)

The assessment is `.superpowers/fable-review/architecture-assessment.md`
in the main checkout (git-ignored); decisions.md holds the three entries
that came out of it ("Performance gate restated", "Visibility buffer is
unconditional", "VRAM budget").

1. Done 2026-11-24: g1 merged (fd3df78), every exit criterion in its plan
   met (the high overview's GPU criterion waived; main's image drops
   there).
2. Done: versioned `.clusters` file names (cache and voxelizer version).
3. Measurement repair (split "cull early", exclude hop frames from 1%
   lows, R5-1 forest walk, explain aa 2.45 vs 1.36 ms, re-measure the
   scene-early split on current trees); several views in one process for
   evidence passes (no such flag exists; `--benchmark --route` is the
   nearest); a startup time breakdown with `--trees` (81 s warm against
   ~11 s with `--placement off`); capture runs exiting non-zero on any
   list drop, a render size other than 2560x1440, or a GPU buffer outside
   device memory (today they only print a warning); timestamps for the
   frames still in flight (backlog); the cascade changes (drop the
   sun-axis re-render trigger, static ground cover out of the cascades).
   Done with g1: the VRAM print (device-local and all heaps, peak at load
   and at the far field), the drop warning over the whole run, voxel
   casters, the caster LOD of one texel in every cascade.
4. Vertex compression (backlog "Cluster vertices are 48 B uncompressed"),
   in a fresh lane once trunk-fix merges (it may change the vertex
   format). It must keep a position path the BLAS build can read:
   `buildBlas` reads float3 positions straight from the vertex pool at
   the vertex stride (renderer_resources.cpp:178-182). No new content
   (forest floor, more species, texture packs) before it lands. Done
   2026-11-25 (merge 3d51440): the TLAS and BLAS only for `--shadows rt`
   and `--reference` (decisions.md "TLAS and BLAS only for ray-traced
   modes"), resident 13956 -> 11998 MiB at the forest edge.
5. Visibility buffer: spec, then build (structural: spec and plan).
6. Ambient occlusion respecified on the resolve: GTAO or the visibility
   bitmask, chosen in that spec.
7. Forest floor: cook (independent of the renderer, once memory headroom
   exists), then placement and the contact look (after AO).
8. Far tile tier spec (instance-less aggregates past ~1 km; aerial gate).
9. Wind on trees and grass with the static-plus-dynamic shadow split.
10. Frame pacing, HDR output (before the final grade is tuned, on a real
    HDR monitor), then the final grade.
11. Water (river; planar reflections plus screen-space).
12. Ambient sound.

Ceremony by size: structural changes (visibility buffer, far tile tier,
wind) get a spec and a plan; everything else gets a backlog row, the
change and a decisions.md entry. Spec status values: draft, decided,
in progress, done.

## Gates

Measured on the benchmark route, 2560x1440 native DLAA, GI on:

- Frame time: route average <= 8.3 ms (120 fps); heaviest segment <= 11.1
  ms (90 fps); 1% low >= 60 fps in every segment, hop frames excluded. 180
  fps is a stretch figure, not a gate. Proof of technique, not a merge
  gate.
- VRAM: resident <= 12 GiB (12288 MiB), the `Loaded: resident` line
  (first draw, far field included); transient peak <= 14 GiB (14336 MiB),
  the `TLAS:` and `Far field:` lines' peaks; device-local, at the showcase
  views, printed by every capture run; every spec states its VRAM delta.
  Main at eb10736 (forest edge, `--trees`): resident 13988 MiB at the end
  of the run (over the gate; the `Loaded:` line reads 13956, the frames
  allocate 32 MiB more; the `TLAS:` line's 13282 predates the far field's
  731 MiB), peak 14160
  MiB at the far field, on the 16 GB card. Main at 3d51440 meets the
  gate at the forest edge (`--trees`): resident 11998 MiB (`Loaded:`),
  peak 13086 MiB (models loading; far field 12411), 290 MiB of resident
  headroom. No content before vertex compression (step 4).
- "Done" is judged by the user's eye on a showcase core of about 4 x 4 km
  (the 0.5 m relief patch), flying included; the world stays 15 x 15 km
  with 2 m terrain and the far tile tier as backdrop.

## Voxel foliage (G1, merged 2026-11-24)

Plan, ledger (every ruling, command and raw number) and decisions:
`plans/2026-11-24-dag-voxel-foliage.md`, its ledger, decisions.md "Voxels
inside the cluster DAG", "Cull list caps from measured need", "Voxel
clusters cast into the cascades", "Caster LOD of one texel in every
cascade".

- The user's bar (2026-11-24): voxels are not noticeable in gameplay; the
  user cannot point at the switch on foot or in flight.
- Known gap: at the overview the voxel crowns cover more of the clearings
  than triangle crowns (crown 25.5% of the clearing mask against 11.5% in
  main ray traced; backlog row).
- Voxel-look fixes merged 2026-11-25 (`--voxel-lod near` reference
  flag; voxels take the sun from the cascades at every distance; solid
  parts of a voxelizing model take its finest voxel; `--shadows rt` voxels
  trace from themselves). Findings: decisions.md "Voxel-look" entries;
  backlog rows for the crown sun-diffuse design, the far-crown cascade
  offset and small-model solid parts. The walk across a hornbeam's switch
  (instance 1621093) is no worse than main b6c1b3b in any segment.
- Next, in order: (1) the trunk close-up fix (pale trunk at 1 m: streaks,
  shards, patches, gloss; diagnosis in the voxel-look lane's round 5);
  (2) a small exposure and sky-light lane; (3) the crown
  opacity/density/sun-diffuse design, with the far-crown cascade offset
  under it and the walk flicker split (still vs moving, sun visibility
  forced to 1).

## State of main (2026-11-24)

Native 1440p; DLSS Super Resolution DLAA default (own stable TAA = fallback,
Ray Reconstruction an A/B); raster cascaded sun shadows default; `--fov` 70
deg horizontal; GI probes default (ground cache, material albedo, foliage
lit where the ray meets it); reference path tracer (`--reference SPP`);
Megascans Trees (alder, beech, hornbeam, 30 variants, traced opaque leaves,
leaf transmission per KHR) under `--trees`; voxel levels inside the trees'
cluster DAGs (G1), casting into sun cascades 0-3, and the far field from
the same voxelizer;
volumetric clouds (outline off by default); valley-anchored haze; geo
channels and causal placement (forest follows land use, scree layer).
Parity route baseline 27.9 fps with the causal forest (accepted; the
performance lane takes it, backlog "Forest render cost after causal
placement").

## Workspace

- Worktrees: the main checkout (branch `g1`), `../meridian-main` (branch
  `main`), `../meridian-lane-b`, `../meridian-canopy`.
- Parked branches (unmerged, on origin; finish or drop when the area comes up):
  - `clouds-25d`, `clouds-25d-mip`: paused 2.5D clouds (volumetric kept).
  - origin only: `ground2` (natural terrain transitions WIP),
    `ground2-cliffs` (non-repeating cliffs WIP); both wait for the forest
    floor work to decide them.
- Archive tags for the five branches deleted on 2026-11-24 (locally and on
  origin); `git checkout archive/<name>` to read one:
  - `archive/far-c`: far-cell raster with two-phase Hi-Z, slower than main
    (the visibility buffer replaces its purpose).
  - `archive/far-m-shadow2`: voxel trees cast into cascades 0-3 (G1 Task 8
    does it).
  - `archive/farm-cpuvox`: CPU voxelizer prototype (tools/cpuvox), absorbed
    by g1.
  - `archive/clouds`: pre-rebase clouds lane, superseded.
  - `archive/meadow-y`: far-field grass match WIP (redone after G1).
- Old lane notes and tools (next.md, RESUME.md, progress ledgers, capture
  scripts) from the 55 removed worktrees: `.superpowers/archive/<worktree>/`
  in the main checkout (git-ignored). The G7 lane helpers (cap.sh, rgb.py,
  oklab-stats.py, forest-clarity.sh, albedo-cap.sh) are in
  `.superpowers/archive/g7/g7/tools/`; the paused `cloudpop` investigation
  in `.superpowers/archive/cloudpop/cloudpop/RESUME.md`.
- Board: https://github.com/users/zye04/projects/1 (issues #1-#13, #16-#32).

## Area notes (detail behind the ordered steps; agreed order within each)

- Canopy lighting (user 2026-11-23): interior sun-path ablation on the new
  terrain (backlog "Forest interior floor"), then front-lit residuals
  (canopy-edge GI sky term, silvery leaves), then the screen-space
  visibility-bitmask pass (GI plan Tasks 6-8; structural: short plan, ask
  first; radius <= 1.5 m, sector-masked probe fill, ~1 ms).
- Lighting after that: G5 (#5) contact/micro shadows tuned on the GI image;
  #30 remainder (intra-crown sun from the prototype voxels, recollision).
- Voxels and leaves (user priority 2026-11-23): seamless voxel switch (G1):
  a seam gate first, then the band with opaque leaves, then the voxels'
  grey speckle; leaf shading: still-camera jitter 1.02 -> 1.35 with traced
  leaves, G18 specular AA, G13 back faces, G15 roughness.
- Terrain: step 4 (fine relief synthesis against the 0.5 m patch) or the
  0.5 m height clipmap (`research/2026-11-23-terrain-triage.md`).
  Deferred minors: no maxPushConstantsSize check (placement push constants
  144 B); the forest-floor bake sits half a sample off its texel centre.
- Image stability, G19 (#22): re-run preset M vs K from scratch
  (`tools/motion-clarity.sh build-release .superpowers/motion/preset-M
  --dlss-preset M`, same for K); then the cloud small-detail smear A/B list
  in `research/2026-11-22-neon-triage.md`. Paused `cloudpop` (sky pops on a
  still camera; suspects: the outline's contrast term, the composite's
  binary motion switch) is archived, see Workspace.
- Performance: parity route (`--benchmark 60 out.csv --route
  tools/routes/parity.route --trees`, `python3 -I tools/bench-route.py`)
  at 27.9 fps against the restated gate (see Gates); G10 (#10) forest
  edge cost.

## Unreal

UE 5.9.0 preview-2 at `~/Apps/UnrealEngine` + Linux Fab and Bridge
plugins; project `~/Documents/Unreal Projects/MyProject` (Python,
EditorScriptingUtilities, GLTFExporter enabled); packs Black Alder,
European Beech, European Hornbeam. Steps: docs/assets.md "Megascans Trees
from Unreal".

## How work runs

Pro account (user, 2026-11-23): work inline in the main thread, no lane
sub-orchestrators or fan-out, lighter workflow (draft, self-review, direct
write; full SDD only for structural changes, after asking), subagents only
when clearly worth it at the lowest tier, fewest GPU runs. The user may run
parallel Claude Code terminals themselves. Ask which account if unclear;
on a larger plan, lanes come back (brief template:
`.superpowers/orch/common-brief.md`, git-ignored; update its dates first).

The user's rules:

- Work fast and right; every milestone message carries an absolute
  local-time ETA. Kill dead ends after one try plus one variant.
- Don't start work the user didn't ask for; when asked to help choose,
  answer directly. Ask before deciding look questions; give a
  recommendation. Prove a look claim with a side-by-side capture.
- When the user reports a visual problem, fix it and verify it in the
  user's conditions (clouds drifting at `--time-scale 1`, mouse-like
  turns, interactive settings), not only static captures.
- Look target: `assets/reference/chlorophyll/` plus
  `assets/reference/target-meadow.png`; clouds stay volumetric.
- Stability over sharpness; judge clarity in motion at user speeds.
- Reports: "status" = running and waiting work only; "overview" = done.
- Usage: the user reports the 5-hour and weekly %; Sonnet for mechanical
  work, Opus for look judgment and risky reviews; never Fable.

Every worktree or parallel run: no AI attribution in commits; commit
messages and temp files inside the worktree (`.superpowers/`), never a
shared scratchpad; every game run through `flock /tmp/meridian-gpu.lock`;
never `pkill`/`killall` or any name-pattern kill (stop only PIDs you
started); `assets/` is a symlink to `/home/zye/Projects/dev/meridian/assets`
(check the placement hash in `docs/reference/cli.md` before measuring); captures at
2560x1440 and GPU ms compared only with identical `Render WxH` lines (the
headless 1440p output when the monitor sleeps, memory native-1440-rule);
DLSS SDK reuse via
`-DFETCHCONTENT_SOURCE_DIR_DLSS_SDK=/home/zye/Projects/dev/meridian/build/_deps/dlss_sdk-src`;
new `.glsl` includes need a cmake re-configure; worktrees beside the repo,
never under `/tmp`; remove a worktree once its branch is merged.
