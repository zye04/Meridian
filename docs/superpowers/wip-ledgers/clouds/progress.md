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
RESUME 2026-11-20 (sub-orchestrator, Lane B): rebasing clouds onto origin/main 4ca0a92 (17 commits incl. placement, TLAS, haze); pre-rebase tag clouds-pre-rebase-2 (5a64f4d). Rebase agent a50b11665b9a68b79 (opus). Scope this dispatch: finish Task 5, Task 6, Task 7, STOP before Task 8 (user sees comparisons).
REBASE 2 done: clouds on origin/main 4ca0a92, HEAD 05685d8 (ledger WIP on top; T5 b3fdadb, T5 WIP fix a009f38). Conflicts textual only. SceneData 1280. Build + 58 tests OK; captures 0 validation errors (1920x1080, monitor off).
⚠️ Found: WIP fix a009f38 carries 3 debug edits: clouds_composite.comp unconditional return; mesh.frag aerial-perspective line deleted; tonemap.frag replaced (rebase took main's AgX+dither). Must be reverted in the fix round.
Task 5 fix round 1 (resume): fresh opus implementer, FIX_BASE b3fdadb (reviewed head 0f7e4c1 rebased), findings task-5-findings-r1.md.
Task 5 fix1 implementer a5aba879cb5553060 (opus), FIX_BASE b3fdadb
User closeout rule added to constraints.md (carry to every dispatch + fold + review).
Task 5 fix round 1 implementer DONE_WITH_CONCERNS (45ff498): debug edits reverted; 2° sun 3.41->0.12 ms (high sun 0.28; worst 0.5 ms at 3-6°); map fades to fallback 6°->3°, off below 3°; fallback calibrated (fair 0.816 map vs 0.829, broken 0.417 vs 0.437); backlog row for <3° no cloud-shaped shadows.
Task 5 fix round 1 re-reviewer a0baeee21fe972e16 (opus), package review-b3fdadb..45ff498-fix1.diff
Task 5: fix round 1/5 (3 addressed + minors addressed, 0 open; commits b3fdadb..45ff498). Re-review clean.
Task 5: minor (docs, folded into fold step): CLAUDE.md Known gaps lacks low-sun limit (<3° fallback only; CLAUDE.md sunset view ~2.9° affected); decisions.md: march beyond-map OD now also cut beyond ~15 km and below 6° (widens Task 7 deferral).
Task 5: minor (deferred → Task 7): fade-band bias at 4-5.5° (near bands brighter); decisions "differs only by cloud pattern" too strong — recheck with another view.
Task 5: complete (commits 024fcff..7c86e0e folded, review clean). Ledger commit d4a9156 on top. prefold tag clouds-t5-prefold=45ff498.
Rebase 3 (onto origin/main 0a0df4e, 23 commits incl. CLAUDE.md restructure, Known gaps -> backlog, placed forest rendering, AO): agent a8f392235b15af3f2 (opus), pre tag clouds-pre-rebase-3=d4a9156.
Rebase 3 done: on origin/main 8eeca40; T5 = 7b88a0f, ledger 0a3d971; SceneData 1344; build+tests OK; captures 0 errors (fair clouds 0.81 ms, weather 0.28). --camera-yaw-rate now shorthand for main's --camera-angular-velocity. Pushed (force-with-lease). Note: stepped/contour bands on fair clouds pre-exist (Task 7 shape/look).
Task 6: dispatched (BASE 0a3d971), implementer a29641419a8bc6eaf (opus)
Task 6: implementer DONE_WITH_CONCERNS (fef195c): 64² octahedral capture; clear MAE 0 vs T5; overcast EV100 12.23 vs clear 15.23; capture +0.07-0.16 ms; clouds+weather 1.23 ms fair ground (over ~1 ms, levers → Task 8 user); above-layer overcast terrain keeps clear ambient (backlog row); dusk ambient warm from too-bright low-sun clouds (→T7).
Task 6 reviewer a044461cc64e85bd7 (opus), package review-0a3d971..fef195c.diff
Task 6: review Approved (spec ✅, 0 Critical/Important). ⚠️ budget 1.23 ms > ~1 ms → user at Task 8 (levers reported).
Ruling: fold minors #1 (irradiance buffer size constant shared), #2 (one decisions line naming uniform in-scatter + grey T approximations), #5 (decisions.md lane-internal names/history → current truth; closeout rule) into fef195c by amend — cheap, same files, closeout rule demands #5 — cost if wrong: one small amend.
Task 6: minor (deferred → final review/backlog): #3 octahedral lower-hemisphere border seam for downward specular; #4 overcast blue cast reduced not neutral (B/G 1.67 vs 1.97); #6 redundant barrier in printAmbient (no action).
Task 6: minors #1 #2 #5 amended (c43abb9): SkyIrradiance struct, approximations line, decisions current-truth. Controller checked diff. Task 6: complete (commits 0a3d971..c43abb9, review clean).
Rebase 4 (onto origin/main 41aec6a: grade 3D LUT tonemap, render/display resolution split, DLSS transparency mask): agent a15c09ac4e2cd2f96 (opus), pre tag clouds-pre-rebase-4=c43abb9. Needed before Task 7 (tonemap changed → look comparisons).
Rebase 4 done: on origin/main 41aec6a. T5 9b23406, ledger 5079a44, T6 90a337d. Clouds targets/dispatch now from renderExtent (render/display split) in each commit — final review must check this (not task-reviewed). Transparency mask: clouds don't write it → backlog row. SceneData 1344. Captures 0 errors incl. --render-scale 0.5. Pushed.
Task 7 carry: contour banding on fair clouds at full render scale (smooth at render-scale 0.5 / 32 jitter phases vs 8) — investigate in look dev.
Task 7: dispatched (BASE 90a337d), implementer a93b7ce71c207bd28 (opus). Ruling: user OK on look comes after the task review; STOP after Task 7 and report comparison paths to coordinator — Task 8 not started (coordinator order) — cost if wrong: an amend after user feedback.
Task 7: implementer DONE_WITH_CONCERNS (782dd03): targets 1 partial, 2 mostly, 3 met-ish, 4 partial, 5 fails numbers (photos themselves violate target 5: tops 4.7-11x sky, bases 2.0-2.8x). Clouds ms down (ground 1.08->0.89). Banding cause = density shelves, fixed.
Task 7 reviewer a5b76a19e21783b46 (opus), package review-90a337d..782dd03.diff. Controller read of sheet: lobed shapes now, but grey/blue-grey flat shading, small numerous cells, pale sky; sunset slate-blue not orange. Not close yet.
Task 7: review Needs fixes — spec ❌ partial (exit condition unmet; user call). Important: I1 photo metric biased (mask drops dark bases, base_at column filter, global vs adjacent sky, fixed px offsets) — base-conflict claim unproven; I2 far/dusk self-shadow relies on Task-5-shape fallback fit (stale; decisions numbers stale); I3 motion sharpness turn sky crop 0.852→0.794 (clouds off 0.855) needs cause or user acceptance.
Task 7: minors folded into round 1 (cheap, same files): CLAUDE.md sunset view missing --weather broken; cloud-compare.sh fail-fast (missing refs, game exit code); ground-bounce coverage sampled at camera per pixel → hoist/uniform; "energy-conserving" octave wording qualified; metrics docstring.
Task 7: minor (deferred → final review): spec text still says octaves 2-3 + edge albedo darkening (spec:219, :371) — stale vs brief; user decides spec edit.
Task 7 fix round 1: resume implementer a93b7ce71c207bd28, FIX_BASE 782dd03.
CHECKPOINT 2026-11-20 (usage limit): Task 7 fix round 1 interrupted; uncommitted work saved as WIP commit on top of 782dd03 (debug calibration hack in clouds_composite.comp/mesh.frag/sky.frag reverted before commit; build-release still has it — rebuild). State per task-7-report.md "## Fix round 1 (interrupted)": I1 done (corrected photo bases 2.1-3.4x, tops 5.9-13x → target-5 conflict holds); cloud-compare.sh fail-fast done; ground coverage hoisted to clouds.groundCoverage done; I3 diagnosed (temporal resolve loses detail; trade with density gain: gain 1/3/6 → ratio 0.836/0.810/0.794) not written up; I2 code in place (local layer, marched=true) but fit NOT refitted (calib-r0: fair -0.05..-0.20, broken +0.05..+0.17).
NEXT: fresh opus implementer (brief task-7-brief.md, report task-7-report.md, findings in ledger Task 7 review line + SendMessage text in this ledger summary): (1) refit kCloudShadowFallbackFit + L with the Task 5 top-down procedure (artifacts/t7/fix1/), (2) write up I3 as a user decision (gain vs motion ratio) + backlog row, (3) docs: CLAUDE.md sunset view --weather broken, decisions qualifier ("energy-conserving" ~1.875x at tau→0) + current calibration numbers, metrics docstring, (4) regenerate artifacts/t7/final/, Step 3 regressions, (5) commit fix, scoped re-review vs I1-I3 + minors, fold WIP+fix into 782dd03 as one "Develop the cloud look against reference photos". Then STOP: report comparison paths to coordinator (user OK before Task 8).
RESUME 2026-11-20 (lane C sub-orchestrator, 3 h box): rebase 5 onto origin/main 1a7ef7d (DLSS module, RR guides, TAA v2, grade, leaf transmission); pre tag clouds-pre-rebase-5=2399906. Conflicts: docs + renderer_frame transitions before the upscaler (clouds leave HDR/motion GENERAL, depth read-only) + CMake shader list. Build, ctest, captures 0 validation errors (TAA and --aa dlss).
Rebase checklist done (commit "Flag clouds in the transparency mask and jitter them with white noise under RR"): mask written by the composite (storage R8, max with coverage), pcg3d jitter under whiteNoise, targets from renderExtent (already), aa pass name in CLAUDE.md. TAA output identical (mean diff 0).
Task 7 fix round 1 closed: I1 (metrics) done earlier; I2 fallback refit k = 1 - 0.72 type^2.6, L 5.5 km (residuals fair -0.05..+0.03, broken -0.01..+0.12; L 3.5 worse); I3 written up in decisions.md as a user decision (fast-turn sky ratio gain 6/3/1.5: 0.848/0.869/0.890, off 0.935); minors done.
Look round 2 (against the Chlorophyll frames): diffuse multiple-scattering term (white tops, light-grey bases, grey overcast), ambient bottom 0.05 under full coverage, gain 3, base 8192 m, detail 64^3 / 256 m + cumulus erosion 0.6, 6 sun steps from 20 m, stratus floor 0.4, 2D cirrus at 9 km. Half-res march tried: no visible gain, +0.65 ms, not kept. Release 1440p clouds: fair 0.91, cirrus view 1.04, broken 1.16, overcast 0.60, sunset 0.99 ms; turn ratio 0.881 (off 0.935).
Scoped review (opus): docs-only Needs fixes (stale calibration numbers, comments, cloud-compare dead grep, metrics docstring) fixed; deferred to backlog: mask over solid cloud (DLSS), cirrus fringe at silhouettes from in/above the layer.
Task 7: complete pending user look approval. Folded: 6f7ef2e "Develop the cloud look against reference photos" + 26f9c05 mask/white-noise commit; prefold tag clouds-t7-prefold. Sheets: .superpowers/sdd/2026-11-19-clouds-weather/artifacts/t7/look/sheet.jpg (+ sheet-<view>.png, make-sheets.sh).
NEXT: user reviews the look sheets (fair, fair-hazy, cirrus, stratocumulus, overcast, sunset) and the I3 trade-off (decisions.md "Cloud look development"). On OK: Task 8 (measurement), rebase onto main, final review (check renderExtent sizing, transparency mask, backlog rows), copy ledger back to .superpowers, drop docs/superpowers/wip-ledgers, ff merge.
