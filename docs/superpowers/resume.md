# Where work stopped (2026-11-20)

Read this first when resuming. Durable state is in git; the SDD ledgers
under `.superpowers/` are local and git-ignored.

## How work runs from 2026-11-21: one orchestrator

The two lanes (A: forests and quality on main; B: clouds on `clouds`) end
here. One orchestrator session owns all open work. It talks with the
user and, per branch, delegates to sub-orchestrators that run
subagent-driven development (implementer, task reviewer, fixer, final
reviewer). The orchestrator:

- tells the user concisely what is being worked on, why, which technique,
  and what it changes in the engine and the game;
- sequences merges into main, so branches never diverge far: rebase the
  next branch onto main right after each merge;
- before each merge, runs a tidiness sweep of the branch against main:
  no duplicated functions or shaders, no unused code, flags, files or
  debug views, no stale docs (CLAUDE.md, decisions.md, specs, backlog);
- keeps `docs/backlog.md` and this file current.

Open branches, in the proposed order (the user may reorder):

1. `clouds` (M4): rebase onto main, finish Task 5's fix round, then
   Tasks 6-8, final review, merge.
2. Then M2a Tasks 8-12 and the rest under "Next after that".

The clouds branch touches the same frame and shading files as the other
two (`renderer_frame.cpp`, `mesh.frag`, `tonemap.frag`, `gpu_types.h`),
so rebasing it after each merge keeps conflicts small.

## Done on main

- M1 (15 km Napf terrain) complete.
- M2a forests: Task 1 (static TLAS), 2 (Megaplants import), 3 (placement
  rules), 4 (GPU placement at load, 2.9M instances), 5 (cell cull,
  prototype BLAS + TLAS fill, placed trees and rocks drawn), 6
  (cluster-group BVH), 7 (GPU BVH traversal; every tree drawn as
  clusters, ~11.5 ms of raster at the forest edge until the far field)
  done, reviewed, pushed.
- Quality pass step 1: sky dithering, summer haze default (AOD 0.10,
  scale height 0.5 km), terrain march lifted above its own cell (contour
  bands at grazing sun).
- Quality pass AO: multi-bounce AO + specular occlusion, per-layer terrain
  AO strength (needle layer 0.5); shadowed needle ground ~11% of sunlit.
- `docs/backlog.md`: everything deferred.

## In progress (WIP branches on origin, not reviewed)

- `clouds`: M4 clouds, see below.

## Where clouds stopped

Branch `clouds` on origin (worktree `../meridian-clouds`), not merged.
Plan: `docs/superpowers/plans/2026-11-19-clouds-weather.md` (8 tasks).

- Tasks 1-4 done and reviewed: weather state + global wind, noise and
  weather map, cloud march, temporal resolve + composite + cloud motion
  vectors.
- Task 5 (cloud shadow map): 0f7e4c1 reviewed as needs-fixes (shadow
  lines unbounded to receivers, ~3.4 ms at a 2 degree sun; pop and ring
  near a horizon sun; beyond-map term uncalibrated). Fix round is
  69066dd, a labeled WIP commit: builds, tests pass, captures unfinished,
  not reviewed, fix report not written. Next: a fresh implementer redoes
  the captures and writes the fix report, scoped re-review, then fold
  into 0f7e4c1 as one commit.
- Then Task 6 (sky + cloud capture, ambient, time-lapse), Task 7 (cloud
  look development against `assets/reference/clouds/`; show the user the
  comparisons before Task 8), Task 8 (Release measurement, budget levers
  to the user before any cut). Task 7's text is in main's plan (b9a19a1),
  not yet on the branch: rebase `clouds` onto main first.
- Merge: rebase onto main, final whole-branch review, fast-forward main;
  move unresolved minors into `docs/backlog.md`.
- Ledger: a copy of the clouds SDD ledger (rulings, deferred minors,
  briefs, reports, Task 7 look-dev notes) is committed on the branch in
  `docs/superpowers/wip-ledgers/clouds/` (5a64f4d). Copy it back into
  `.superpowers/sdd/2026-11-19-clouds-weather/` and delete the folder
  before merging. Capture artifacts (477 MB) stayed local.
- Task 7 needs the reference photos in `assets/reference/clouds/`
  (git-ignored; copy them across with `assets/`).

## Next after that

M2a Tasks 8 (voxel far-field prototype: stop and
show the user its gates; option B is the fallback), 9 (far-field
completion), 10-11 (mid-range impostors), 12 (gate: forest views
<= 16.7 ms with shadows). Far field decided 2026-11-20
(`docs/superpowers/specs/2026-11-20-far-field-design.md`). Quality pass step 2 (shadow denoiser + TAA
sharpness) needs a spec first. Then M2b (forest lighting), M3 (meadow).

## Process

Subagent-driven development per plan task: brief from the plan, fresh
implementer, task review, push after a clean review. Every game run goes
through `flock /tmp/meridian-gpu.lock`. `/tmp` is wiped on reboot: keep
artifacts under `.superpowers/sdd/<plan>/artifacts/`.
