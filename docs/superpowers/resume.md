# Resume here (entry point; updated 2026-11-22)

Read this file first. Weekly usage resets **Tuesday 2026-11-24 05:00**.

## Session 2026-11-22 (Pro account, inline work, no lanes)

Pro-plan rules while on that account: work inline in the main thread, no
lane sub-orchestrators or fan-out, lighter workflow (draft, self-review,
direct write; full SDD only for structural changes, after asking),
subagents only when clearly worth it at the lowest tier, fewest GPU runs.
The user may run parallel Claude Code terminals themselves.

Landed on main: `tools/motion-clarity.sh` (4b2f454: jitter = frame 120 vs
121 at the same pose while moving, error = motion vs still), stable TAA
(5789387, `Effects::stableTaa`, absorbs the `native` branch's 3x3
reconstruction: drop that branch), DLSS DLAA as the default AA (a5c8f43).
User preference: stability over sharpness (memory).

G19 (#22) plan, this lane ("image-stability"):
- Step 0 done (baseline under TAA: `.superpowers/motion/baseline-main/`).
- Step 1 done (stable TAA, then DLAA chosen; decisions.md 2026-11-22).
- DLAA baseline done: `.superpowers/motion-baseline-dlaa.txt`.
- Step c done (a28c484): `Effects::cutoutMipBias`, default +1 on the masked
  depth pass (ground jitter -8..-11%, forest +1.1 ms; decisions.md). -1 and
  +1 runs in `.superpowers/motion/cutout-{m1,p1}/`.
- Next, step 2: cloud small-detail smear (quarter-res march + history,
  measured on a crop of cloud detail, not the whole sky region): A/B
  half-res march outside the layer, shorter history in motion, outline in
  motion (`cloud-outline=0` doubles as the G16 check).
- Even under DLAA a moving frame differs ~6x more than a still one:
  foliage input is the larger remaining source.

Parallel terminals the user opened (check with them / `git worktree list`):
- G20 (#23) cloud shadows stepping: worktree `../meridian-g20`, branch `g20`.
- Leaf geometry design (cards -> opaque triangles at cook time), folded
  into `specs/2026-11-21-materials-fidelity-design.md`: docs only.

## The work list

- **Programme:** `docs/superpowers/specs/2026-11-21-chlorophyll-parity-programme-design.md`
  (why, goals, anti-goals, two rules: each frame correct on its own; cost
  follows visible pixels; 180 fps native 1440p on the 4080 as proof;
  phase 0 + five pillars; GitHub tracking).
- **Board:** https://github.com/users/zye04/projects/1 (Acknowledged ->
  Investigating -> Planned -> Implementing -> Verified). Issues #1-#12 =
  audit gaps G1-G12, #13 matched views, #14 route (closed), #15 metric
  (closed), #16-#21 = G13-G18, #22 = G19, #23 = G20.
- **Evidence:** `docs/superpowers/specs/2026-11-21-look-gap-audit.md`
  (G1-G20 with screenshots in `docs/images/look-gap-2026-11-21/`),
  `2026-11-21-chlorophyll-baseline.md` (graded facts about his engine,
  frame inferences, our capabilities), research
  `docs/superpowers/research/2026-11-21-{A..E}-*.md`, matched views +
  resemblance numbers `docs/superpowers/research/matched-views/`.

## Next, in this order

1. **G19 (#22) motion smear/jitter** on clouds, trees, textures (the
   user's top complaint) and **G20 (#23) blocky cloud shadows**: measure
   with a moving camera at user-like speeds first (the flicker metric so
   far was static), then fix per rule 1. CE's alpha-grass mip bias and
   motion smear mask (baseline F16) are A/B candidates; his AA is unknown.
2. **Regression checks (cheap A/Bs):** G16 (#19) blocky distant clouds vs
   `--effects cloud-outline=0`; G17 (#20) terrain banding vs the
   brute-force march / pre-aaab45b.
3. **Leaf material** (Pillar 1, #18 G15) first: it feeds G13 (#16 black
   back faces), G14 (#17 giant cards), G18 (#21 fireflies). Spec
   `2026-11-21-materials-fidelity-design.md` (draft: user confirms 3
   decisions) -> plan.
4. **Reconcile WIP branches** against their issues: `native` (absorbed by
   stable TAA 5789387: drop it), `far-m-shadow2` (distant tree shadows, #11),
   `far-c` (far-cell occlusion, #10). Finish or drop each.
5. Re-run the parity route (`--benchmark 60 out.csv --route
   tools/routes/parity.route --trees`, `python3 -I tools/bench-route.py`)
   on the Megascans Trees + 70 deg FOV; find the 20-26 fps 1% low hitches.
6. Phase 0 Task 4, G7 grey image (#7): plan
   `docs/superpowers/plans/2026-11-21-phase0-trustworthy-judgment.md`.
7. GTAO (#2): spec `2026-11-21-gtao-design.md` (draft: user confirms 3
   decisions) -> plan -> implement.
8. Matched views refined at 70 deg (#13); motion gates re-anchored at 70.

## State of main (2026-11-21)

Native 1440p, DLSS DLAA default since 2026-11-22 (own stable TAA = fallback); raster sun shadows default;
`--fov` default 70 deg horizontal; Megascans Trees (alder, beech,
hornbeam, all variants) under `--trees` (forest edge ~16 ms, #10);
voxel trees; clouds with outline (default 1); valley-anchored haze; GI
probes off by default; benchmark route + resemblance metric tools.

## Unreal

UE 5.9.0 preview-2 at `~/Apps/UnrealEngine` + Linux Fab and Bridge
plugins; project `~/Documents/Unreal Projects/MyProject` (Python,
EditorScriptingUtilities, GLTFExporter enabled); packs Black Alder,
European Beech, European Hornbeam. Steps: docs/assets.md "Megascans Trees
from Unreal".

## Decisions waiting for the user

1. GTAO spec's 3 decisions; materials spec's 3 decisions.
2. GI default (off until forest cost and motion gates pass).
3. The saved grade file overrides the starting grade (G panel Reset +
   Save).

## Housekeeping

~48 old worktrees sit beside the repo (`git worktree list`). Branches
with unmerged commits: `native`, `far-m-shadow2`, `far-c`, `clouds-25d`
(paused 2.5D clouds), `meadow-y`. Everything else is merged or obsolete:
remove those worktrees only after the user OKs a list.

## How work runs (still valid)

One orchestrator session is the user's only interface. It does no
implementing, reviewing or research itself: it dispatches one
sub-orchestrator per lane (background agent, self-contained brief) and
relays reports and user decisions. Each sub-orchestrator runs SDD
(Sonnet implementers, Sonnet reviewers, Opus only for look judgment and
risky reviews; never Fable) in its own worktree beside the repo (never
under `/tmp`), and merges to main by rebase + fast-forward after review.

The user's rules:

- Time: work fast and right. The user may set deadlines and ask for more agents: lanes then cut scope to a
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
  `assets/reference/chlorophyll/` plus `assets/reference/target-meadow.png`
  (clouds stay volumetric by the user's choice). Native 1440p, own TAA, no
  upscaler is our direction, not a stated CE fact (F16); raster on the default
  path.
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
`/home/zye/Projects/dev/meridian/assets` (check the link and the placement hash
in CLAUDE.md before measuring); new `.glsl` includes need
a cmake re-configure before shaders rebuild; ccache + mold are automatic.

Reports: "status" = running and waiting work only; "overview" = done.

- Usage: the user reports the 5-hour and weekly %; Sonnet for implementers,
  reviewers and research, Opus only for look judgment and risky reviews;
  one or two lanes at a time near the weekly limit.
- Be judgmental: no ceremony for small changes, kill dead ends after one
  try plus one variant, Debug once before merge (lane brief template:
  `.superpowers/orch/common-brief.md` in the main checkout, git-ignored;
  update its dates before reuse).

