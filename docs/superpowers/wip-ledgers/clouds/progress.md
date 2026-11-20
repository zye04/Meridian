# SDD ledger — plan: docs/superpowers/plans/2026-11-19-clouds-weather.md

Spec: docs/superpowers/specs/2026-11-19-clouds-weather-design.md
Worktree: /home/zye/Projects/dev/meridian-clouds (branch clouds, from origin/main 3e99e05). Baseline: build OK, tests pass, capture 0 validation errors.

## Pre-flight scan
| Pair / task | Shared file/interface | Finding |
|---|---|---|
| T1->T2 | WeatherGpu/history, windOffset (wind.glsl) | consistent; T2 consumes T1 names |
| T1->T4 | wind/previousWind in SceneData | T4 reads previousWind produced in T1: ok |
| T2->T3 | clouds.glsl cloudDensity, weatherMapAt, GpuClouds | ok |
| T3->T4 | march outputs (RGBA16F + R32F depth), bilinear composite replaced | ok |
| T3/T5 | clouds_march.comp light march; T5 adds shadow-map use | ok, additive |
| T5->T6 | cloud shadow in mesh.frag; capture uses coarse march | ok |
| T1/T6 | sky irradiance buffer meaning kept | ok |
| T4 | --capture-sequence added in T4 but T6 uses it | ok (T4 before T6) |
| T1 | Vec3 used for xz wind (no Vec2 type) | consistent with engine |
| T5 | tests file "weather_test.cpp (or clouds_test.cpp)" | Ruling: use tests/clouds_test.cpp for shadow tests — separation by subsystem — trivial if wrong |
| T7 | roadmap edits | report conflicts to user, don't edit |
| all | GPU lock on every game run; no AI attribution | carried in every dispatch |

## Tasks
Task 1: dispatched (BASE 3e99e05)
Task 1 implementer: ac8b7e38a5f6fed74 (opus)
Task 1: implementer DONE_WITH_CONCERNS (7cede13). Concerns: no CPU/GPU parity test (GLSL unused until T2); GPU wind offset float grows unbounded (~6 cm precision after 864 km) — Ruling: carry to Task 2: wrap the offset by the noise tile period where it samples tileable noise — cheap, precision bounded; cost if wrong: drift visible after a day of play. Extras (Weather::time, public constants, default dir {1,0,0}, two Wikipedia sources) left to review.
Task 1 reviewer: a9f4940c5c53f055c (opus), package review-3e99e05..7cede13.diff
Task 1: complete (commits 3e99e05..7cede13, review clean). ⚠️ trailer checked: none. Minors parked: calm newest-span drift (≤1 interval), float absolute travel in CPU history, kGustPhasePeriod duplicated C++/GLSL, --weather CLI fail-late, mid-blend retarget jump, unused seed_. Ruling: carry "kGustPhasePeriod via GpuWind or assert" and "--weather fail-fast" to Task 2 dispatch as small extras — cheap, touches same files.
Task 2: dispatched (BASE 7cede13), implementer a729bc33f42f02252 (opus)
Task 2: implementer DONE_WITH_CONCERNS (656a033). Rulings:
- Ruling: overcast gaps — coverage becomes a soft threshold (smoothstep around 1-coverage with an edge width) so coverage 1 is solid; carried into Task 3 (where visuals are tuned) — spec intent "threshold remap, cells grow from cores" kept — cost if wrong: cell edge look differs, tunable.
- Ruling: Task 4 gets a CPU-computed (double) per-frame cloud wind displacement in GpuClouds instead of differencing floats — cost: one vec3 field.
- Analytic vertical profiles instead of a type texture: left to reviewer; spec's builder latitude.
Task 2 reviewer: ac005d25aef56a076 (opus), package review-7cede13..656a033.diff
Task 2: review Needs fixes — Important #1 double coverage threshold (partly plan-mandated: spec remap(n,1-c,1)).
Ruling: coverage is thresholded once, in the weather map, as a saturating soft threshold R = saturate(remap(n, 1-c, 1-c+w)) (w = edge width) so c=1 is solid and c is close to the sky fraction; cloudDensity uses the spec's plain product (R x profile x eroded noise), no second 1-R threshold; decisions.md states what was measured — spec intent ("threshold remap, cells grow from cores", "overcast flat") kept, formula literal changed — cost if wrong: cell edge look, tunable in Task 3.
Ruling (Minor #1): analytic profiles accepted over a type texture, but erosion must vary with type (spec: density and erosion by type) — folded into this fix round, cheap.
Task 2: minor (deferred → folded into round 1, cheap & same files): #3 type clamp consistency map vs fallback; #4 stale GENERAL layouts in cloudSet; #5 windOffsetModulo duplication.
Task 2: minor (deferred): #2 edge-continuity check weak → carry to Task 3 capture across window boundary; #6 hard-coded noise mean/deviation constants; #7 fallback cost → Task 3 budget; #8 → Task 4 (already ruled); #9 pre-existing driverLeak warnings.
Task 2: fix round 1/5 (5 addressed, 0 open; commits 656a033..43da194)
Task 2: complete (commits 7cede13..43da194, review clean)
Task 2: minor (deferred): coverage curve c(1+w)-w/2 vs "c"; broken measured 0.648 vs 0.6725 predicted. Carry to Task 3: stratus overcast may need a shape floor to read as a flat sheet; capture across weather-map window boundary.
Task 3: dispatched (BASE 43da194), implementer ae056a5131b2eb1c7 (opus)
Task 3: implementer DONE_WITH_CONCERNS (3941e8c). Concerns: base shape remapped over measured range + per-type cut (not a coverage threshold; reviewer to judge vs Task 2 ruling); dither streaks → Task 4 resolve; clear sky costs 0.33 ms (empty-sky march + fallback) → carry to Task 7 levers (e.g. coverage-max early-out); Debug timings only. Controller looked at fair ground/above captures: plausible, streaks visible.
Task 3 reviewer: a632a150932b0be2b (opus), package review-43da194..3941e8c.diff
Task 3: review Needs fixes — Important: shape cut includes coverage (second coverage threshold vs Task 2 ruling). Fix per reviewer (coverage outside the cut) + decisions.md wording.
Task 3: minors folded into round 1 (same files, cheap): step size on long segments (baseStep=segment/64 dominates → km steps, likely striping); skip light march when sun contributes 0; kDepthStages consistency.
Task 3: minor (deferred → Task 7 levers): march envelope 1.8-4.5 km for every weather; clear-sky cost floor 0.33 ms. Deferred: kShapeRange measured constant; beyond-128 km haze heuristic.
User 2026-11-19: M1 complete on main (8bf4871); rebase clouds now (after Task 3 fix round commits); Lane A started M2a on main: pull --rebase before each push.
Task 3: fix round 1/5 (4 addressed, 0 open; commits 3941e8c..5fc9df2)
Task 3: complete (commits 43da194..5fc9df2 pre-rebase = d2fc14b..30e8597 post-rebase, review clean)
Task 3: minor (deferred): light march continues below layer bottom (wasted samples); stratus at low coverage now near-uniform (check broken capture); erosion acts as soft second threshold at low coverage (note in decisions); last step overshoots scene depth; comb striping from above → Task 4; cost up (fair ground 0.81-0.89 ms Debug) → Task 7 levers.
REBASE: clouds rebased onto origin/main 8bf4871 (M1 complete). New SHAs: 1829586 T1, 977d6e3+d2fc14b T2, 4ae3890+30e8597 T3. Conflicts: decisions.md (kept both), gpu_types.h SceneData size (1088, then 1184; build verifies), CLAUDE.md views (kept both). Pre-rebase tag clouds-pre-rebase. Build + ctest OK.
Post-rebase verification: fair/overcast/above/clear 0 validation errors; clear vs --clouds off MAE 3.7e-5 (noise); fair ground image OK; Debug clouds ms fair 0.84 overcast 0.26 above 0.69 clear 0.37.
Task 4: dispatched (BASE 30e8597), implementer ae035b025e1a902e0 (opus)
Task 4: implementer DONE w/ minor concerns (9cca0e4): faint static 4-px grid on horizon clouds (quarter-res upsample); motion vectors only measurable in time-lapse; Debug ms ground 0.8-1.0, inside 1.5-1.6.
Task 4: review Needs fixes — Important: silhouette-in-front-of-moving-clouds check uninformative (still camera, radial drift, whole-frame metric); decisions cites 1.000. Fix: lateral-drift silhouette crop run + crop-based on/off baselines; correct decisions.
Task 4: minors folded into round 1 (docs/cheap): record inside-layer disocclusion gap (or fix if trivial), exposure-rescale gap for cloud history, 4-px horizon grid with levers in Known gaps/decisions.
Task 4: minor (deferred): Catmull-Rom 4x4 vs 2x2 disocclusion gather; per-invocation inverse(VP); one setWeather per draw contract unenforced.
Task 4: round 1 implemented (4f0d61f): lateral-drift silhouette crop 1.001; crop ratios turn sky 0.698 vs 0.685, band 0.728 vs 0.733; toward-layer ~equal; inside-layer disocclusion fixed via -1-d encoding (untested path); known gaps documented. Re-review a65acd003962de172 dispatched.
Task 4: controller-confirmed gap (⚠️→real): visible static horizontal banding (step aliasing) + 4-px grid on mid-distance clouds after accumulation (scratchpad/t4/fix/comb.png, viewed by controller). Ruling: fix in Task 4 round 2, not a known gap — photorealism priority and the spec's "no shimmer/smear" anti-goal; per-frame jittered steps + accumulation should converge; if it doesn't, the jitter/history is wrong — cost if wrong: one more round.
Task 4: fix round 1/5 (3 addressed, 0 open; commits 9cca0e4..4f0d61f)
Task 4: minor (deferred): cut texels may accept uncut history on inside-layer path (untestable now); composite d=0 cut uses resolved not forced clear; check composite 4x4 fallback tests d>=0 (out of diff); permanent restart band along static silhouettes (no accumulation at edges).
Task 4: round 2 (banding/grid) in progress with implementer.
Task 4: round 2 implemented (2a924dd): cause = jitter only at start, collapsed on evenly split long rays; per-step jitter + Catmull-Rom composite + tent restart; grid 0.384→0.158; ms ground 0.91→1.08 Debug. Controller viewed before/after: bands gone; underside ribbons remain (shape, deferred).
Task 4: fix round 2/5 (2 addressed, 0 open; commits 4f0d61f..2a924dd)
Task 4: complete (commits 30e8597..2a924dd, review clean)
Task 4: minor (deferred): Catmull-Rom overshoot at cloud/sky edges (clamp to central 2x2 min/max if visible); underside ribbons not in CLAUDE.md Known gaps (only decisions) → Task 7 sweep; 16-tap upsample cost → cheaper 9-tap via bilinear (Task 7 lever); faint lines in wall_tent.png sky (uninvestigated) → final review.
Task 5: dispatched (BASE 2a924dd), implementer a18caafdb9463fb2c (opus)
PLAN CHANGE (user 2026-11-19): new Task 7 Cloud look development inserted before measurement (now Task 8); plan edited on main (docs-wt), branch gets it at next rebase. Task briefs for 7/8 extracted from docs-wt copy. Refs copied to assets/reference/clouds/. Notes agent a7978ba5af86ac53f distilling sources → scratchpad/cloud-look-notes.md.
Notes ready: scratchpad/cloud-look-notes.md (for Task 7 dispatch).
Task 5: implementer DONE w/ concerns (0f7e4c1): weather pass 0.34 ms ground, 3.4 ms at 2° sun (80 km lines through fallback); Beer test loosened after RED→GREEN failure (OD overestimate 7%); beyond-map transmittance 1-coverage (dark?) +0.15 ms early scene on high views.
RESUME after PC shutdown (2026-11-20): /tmp scratchpad wiped (captures, cloud-look-notes.md, docs-wt). Worktree + ledger intact; Task 5 committed 0f7e4c1, review not yet dispatched. Durable artifacts now go to .superpowers/sdd/<plan>/artifacts/.
Resume: Task 5 reviewer ae6dd7904013e50e2 dispatched; notes re-distill afb93f93eb45b0eb9 → artifacts/cloud-look-notes.md
Task 5: review Needs fixes — Important: (1) shadow lines unbounded to receivers → 3.4 ms at 2° sun; (2) 0-0.3° sun: map off but fallback darkens beyond → pop + seam; (3) beyond-map 1-coverage uncalibrated.
Task 5: minors folded into round 1: Beer test add far depths + uniform-column OD 5% check; Frisvad singularity → nadir + north-horizon test; decisions reach figure.
Task 5: minor (deferred → Task 7): march adds no OD beyond map (far clouds brighter).
Task 5: original implementer lost in shutdown → fresh opus implementer for fix round 1 (report file is memory).
Task 5 fix round 1: implementer aa7ca772cc11d8e40 (opus, fresh), FIX_BASE 0f7e4c1
Notes regenerated: artifacts/cloud-look-notes.md (durable).
Controller: docs/backlog.md on main (64b263a Lane A, 3790b68 mine). At merge: move unresolved deferred minors into it; final review checks it.

## Pause 2026-11-20 (user switching SSD; resume next session)
Next on resume:
1. Task 5 fix round 1 commit (on top of 0f7e4c1): review-package 0f7e4c1..HEAD, scoped opus re-review (3 Important + folded minors), then mark Task 5 complete.
2. Task 6 (sky + cloud capture, ambient, time-lapse): extract brief with task-brief.
3. Task 7 look dev: brief from origin/main's plan (b9a19a1) or rebase first; artifacts/cloud-look-notes.md; refs in assets/reference/clouds/. Show user comparisons, get OK.
4. Task 8 measurement, then rebase onto main, final review, ff merge. Move unresolved minors to docs/backlog.md.
Every implementer dispatch: GPU flock, zsh "$@" note, durable artifacts dir, no trailer.
Task 5 fix round 1: implementer stopped for machine switch; uncommitted work saved as WIP 69066dd (builds, tests pass; capture verification and fix report unfinished — the fresh implementer must redo the captures and write the fix report).
