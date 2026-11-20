# Where work stopped (2026-11-20)

Read this first when resuming. Durable state is in git; the SDD ledgers
under `.superpowers/` are local and git-ignored.

## Done on main

- M1 (15 km Napf terrain) complete.
- M2a forests: Task 1 (static TLAS), 2 (Megaplants import), 3 (placement
  rules), 4 (GPU placement at load, 2.9M instances), 6 (cluster-group BVH)
  done, reviewed, pushed.
- Quality pass step 1: sky dithering, summer haze default (AOD 0.10,
  scale height 0.5 km), terrain march lifted above its own cell (contour
  bands at grazing sun).
- `docs/backlog.md`: everything deferred.

## In progress (WIP branches on origin, not reviewed)

- `wip/m2a-task5`: M2a Task 5 (cell cull, TLAS fill, placed branch of
  `loadInstance`). Builds; not verified. Resume it with the Task 5 brief in
  `docs/superpowers/plans/2026-11-19-forests.md`, then squash into one
  commit. Carried items for Task 5: ridge capture backing the ground-fit
  decision (trees at 1-2 km); keep the moss rock sets' authored offsets;
  Task 5 is exempt from the 0-drop rule (work items overflow until Task 7).
- `wip/quality-shadows`: commit 8e530ab (multi-bounce AO + specular
  occlusion, complete, needs review) and 1c56eb8 (per-layer terrain AO
  strength, unfinished; may contain a temporary luminance debug view).
  Goal: shadowed ground 10-20% of sunlit (now ~6%). Next: finish
  per-layer AO strength (forest_leaves_04 at 0.5), measure, review, merge.
- Lane B (separate instance): M4 clouds on branch `clouds`
  (worktree ../meridian-clouds), at its cloud look-development task.

## Next after that

M2a Tasks 7 (GPU BVH traversal), 8-9 (impostors), 10 (gate: forest views
<= 16.7 ms with shadows). Quality pass step 2 (shadow denoiser + TAA
sharpness) needs a spec first. Then M2b (forest lighting), M3 (meadow).

## Process

Subagent-driven development per plan task: brief from the plan, fresh
implementer, task review, push after a clean review. Every game run goes
through `flock /tmp/meridian-gpu.lock`. `/tmp` is wiped on reboot: keep
artifacts under `.superpowers/sdd/<plan>/artifacts/`.
