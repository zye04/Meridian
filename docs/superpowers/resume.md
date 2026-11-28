# Resume here (entry point; updated 2026-11-28 midday)

Read this file first, ask the open questions below, and update the file
before ending the session. It holds current truth only: open questions,
branches, the order of work, the gates, and the state of main. Session
history lives in git (`git log -- docs/superpowers/resume.md`) and in
`docs/decisions.md`. Known issues: `docs/backlog.md`.

## Open questions for the user

At most three, each answered in chat (the user opens no documents); the
agent writes the answer into the spec or here. Images are in the main
checkout under `.superpowers/fable-review/img/` (git-ignored).

1. Tree assets: the canopy from the air reads soft because the interior
   trees' crowns are small; a placement rule cannot give broad tops
   without low branches. Wanted: forest-grown variants with broad crowns
   and clean lower trunks (buy a pack, export from Unreal, or leave).
   Tools must be script-driven (SpeedTree was rejected because the agent
   could not drive its GUI). The one-tree premise test is still free.
2. `--forest-picks open` as the aerial-shot mode? Better from the air (s1
   28.26 -> 23.44 ms, separate round crowns), but closed and darker at eye
   height (22.57 -> 27.13 ms): a flag, not a default. Voxel coverage mode
   (hash against area-pixel-phase, ~2x) stays a look call; asked with the
   whole-frame sheets.

Decided 2026-11-28: merge after a clean independent review; look changes
land on the director's judgment when they measure closer to the references
or the path tracer, the old behaviour behind a flag. Still the user's:
purchases, the tree source, destructive actions. Parked: DLSS 5 NR via
Optiscaler as a look reference (never shipping); the Unreal comparison.

## The lighting target

Our own `--reference` path tracer on our trees at the same camera is the
same-scene reference. The earlier "crowns 3x darker in the tracer" was an
exposure mismatch (raster 120 frames EV 12.59, tracer 30 frames EV 13.66);
at equal EV, linear, raster / tracer is 1.10 on the near canopy at o4.
Capture rule for any raster / tracer pair: same `--frames` warm-up on both
sides, quote both `EV` lines. Measured causes of the remaining gaps (each
by capture, not by reading text; six text-born causes were wrong):

- Soft crowns from the air: cascade maps (1.57 m texels at 420 m) cannot
  shadow voxel crowns; the sun leaks onto shade sides (1.88x the tracer)
  and ambient is 0.41x. `--shadows rt` fixes it at +21.8 ms; cascade 3 at
  4096 spikes in motion. Lanes: `voxel-shadow-lookup`, `crown-light-pair`.
- Leaf-scale contrast at 2 px (ours 6.4, tracer 9.6, 1.jpg 10.1) is a
  lighting gap, not geometry: the per-leaf sun is filtered away.
  Experiment `voxel-leaf-sun` follows the crown pair.
- Blue shade is haze in-scatter in the first 2 km (its amount, not a
  missing shadow term). The forest interior had been lit by that veil: the
  raster's sky light under canopy is 0.74 of the tracer's (probes see the
  canopy as a uniform medium). Lane `interior-ambient`.
- Pale sky is AgX shoulder desaturation (linear C 0.091 -> display 0.024).
  Lane `sky-chroma` (`Grade::highlightChroma`).
- Far ground is flat past the blade fade (120 m); the placeholder mud, soil
  and clover roles show through; it needs canopy shading that depends on
  the view. Lane `far-ground`.
- Not levers: larger voxels, per-brick occlusion (12-18 % from the air),
  forest picks by edge, shadowed haze (backlog records each).

Default look on main (ee101c1): haze `clear` plus grade G4 (7000 K, EV
-0.8, offset 0.010, contrast 1.4, greens 0 deg x1.2, blues x0.6). The
previous look: `--haze summer --grade tools/grades/starting-2026-11-28.json`.
Re-run `tools/tone-match.py` once after the crown fill and interior
ambient land, for the final grade.

User's criterion: forms read as solid at every distance ("structure, not
busy"); measure with `tools/band-contrast.sh` plus a squint pair. Ours
is soft at pixel size (o4 2 / 6 px 5.5 / 7.3 against 10.1 / 11.2) and
equal at crown size.

## Branches (2026-11-28 midday; all pushed unless noted)

| Branch | Worktree | State |
|---|---|---|
| `main` ee101c1 | `../meridian-main` | leaf light B, haze `clear`, palette light1 and the new default look landed today, each reviewed. The main checkout stays on `g1` |
| `voxel-shadow-lookup` f3ec373 + `crown-light-pair` 36b11b0 | `../meridian-crownlight` | lane `pt`. Review-clean bar a doc-fix commit and the off-cost figure (`reviewers/crown-fill-2-review`); fix check, then merge both. Next: `voxel-leaf-sun` experiment (stability gates first), re-fit the fill strength after interior-ambient |
| `far-ground` f34e176, def64c2, 4beb453 | `../meridian-ground` | crawl gate and hand-over passed; final proof and one launch under the new default, then review and merge |
| `interior-ambient` | `../meridian-fix` | premise measured (sky through canopy 0.74x); building the clumping correction on probe rays; then review and merge. Parked beside it: `far-cell-occlusion` |
| `sky-chroma` | `../meridian-haze` | CPU side done (`Grade::highlightChroma`); rebase onto ee101c1 and a 2-launch proof; the default value is set with the grade |
| `visibility-buffer` d926392 (tag `vb-pre-rebase` 2ff92cd) | `../meridian-vb` | midpoint review fixes done, off = main exact, sync clean; gates rerun paused until the GPU is free; then rebase onto main, gates, fix check, merge stages 1-2 (flag off), stage 3 after |
| `far-tier` 1deda3b | `../meridian-far` | far tile tier per `specs/2026-11-26-far-tile-tier-design.md` (on that branch); waits for the user's verdict and the visibility buffer; no merge |
| Closed | | `pt-targets`, `port-b-look`, `palette-light1`, `clear-haze`, `look-default`, `fix-leaf-phase`, `voxel-bricks` (merged or finished; the forest-edge picks run found no gain); `shadowed-haze` kept as a recorded experiment (backlog); `far-light`, `trees-dense` closed earlier |
| `crown-fill` 505eb84 and older (`scene-broadleaf`, `crown-light`, `voxel-crown-9`, `fix-light-temporal`, `canopy-backlit`, `vb-stage0`, `trees-domes`) | see Workspace | held or parked; detail in `.superpowers/fable-review/handover-2026-11-27-midday.md` |

Run-to-run floor with placement on: 0.2-0.6/255; only `--placement off`
is exact. Lane report shape: see CLAUDE.md.

## Ordered next steps (updated 2026-11-28 midday)

1. Merge the crown light pair (after its fix check), then `far-ground`,
   then `sky-chroma`.
2. `interior-ambient`; then re-fit the crown fill strength and re-run
   `tools/tone-match.py` once for the final grade.
3. Visibility buffer: gates, fix check, merge stages 1-2 (flag off); stage
   3 (voxels) after.
4. `voxel-leaf-sun` verdict; then `far-cell-occlusion`.
5. For the user by eye, whole frames beside the references: tree assets
   with broad forest-grown crowns; `--forest-picks open` as the aerial
   mode; voxel coverage mode.
6. Later (unchanged): bark material lane; far wood triangle cost; AO
   respecified on the resolve; forest floor; far tile tier verdict; wind on
   trees and grass; frame pacing, HDR output, final grade; water; sound.

Ceremony by size: structural changes (visibility buffer, far tile tier,
wind) get a spec and a plan; everything else gets a backlog row, the
change and a decisions.md entry. Spec status values: draft, decided,
in progress, done. Unverified on main: long Debug run, route gates, 1 %
lows (backlog).

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
  Main at d8440c4 loads the default scene (`--trees`) at 11370 MiB
  resident; `far-tier` loads it at 12033 MiB (255 MiB under the gate).
- VRAM figures predate vertex compression; the next GPU lane updates them
  from its `Loaded: resident` line.
- "Done" is judged by the user's eye on a showcase core of about 4 x 4 km
  (the 0.5 m relief patch), flying included; the world stays 15 x 15 km
  with 2 m terrain and the far tile tier as backdrop.

## Voxel foliage (G1, merged 2026-11-24)

Plan, ledger and decisions: `plans/2026-11-24-dag-voxel-foliage.md` and its
ledger; decisions.md "Voxels inside the cluster DAG", "Voxel-look" entries,
"Caster LOD of one texel in every cascade". The user's bar: voxels are not
noticeable in gameplay; the switch cannot be pointed at on foot or in
flight. Known gaps are backlog rows (crown coverage at the overview, hidden
cut, thin structure).

## State of main (ee101c1)

Native 1440p; DLSS Super Resolution DLAA default (own stable TAA =
fallback, Ray Reconstruction an A/B); raster cascaded sun shadows default;
GI probes default; reference path tracer (`--reference SPP`); Megascans
Trees under `--trees`; voxel levels inside the trees' cluster DAGs (G1),
casting into sun cascades 0-3, far field from the same voxelizer; volumetric
clouds; valley-anchored haze; geo channels and causal placement;
crown-specular and foliage specular antialiasing (on by default); leaf
light B and the new default look (see "The lighting target"); the
visibility buffer design (spec only). Parity route baseline 27.9 fps with
the causal forest (accepted; backlog "Forest render cost after causal
placement").

## Workspace

- Worktrees: `git worktree list` (table above names the live ones; remove
  one once its branch is merged).
- Parked branches (unmerged, on origin; finish or drop when the area comes up):
  - `clouds-25d`, `clouds-25d-mip`: paused 2.5D clouds (volumetric kept).
  - origin only: `ground2` (natural terrain transitions WIP),
    `ground2-cliffs` (non-repeating cliffs WIP); both wait for the forest
    floor work to decide them.
- Archive tags `archive/*` hold merged or superseded branches (`git tag -l
  'archive/*'`; `git checkout archive/<name>` to read one).
- Old lane notes and tools from removed worktrees: `.superpowers/archive/<worktree>/`
  in the main checkout (git-ignored).
- Board: https://github.com/users/zye04/projects/1 (issues #1-#13, #16-#32).

## Unreal

UE 5.9.0 preview-2 at `~/Apps/UnrealEngine` + Linux Fab and Bridge
plugins; project `~/Documents/Unreal Projects/MyProject` (Python,
EditorScriptingUtilities, GLTFExporter enabled); packs Black Alder,
European Beech, European Hornbeam. Steps: docs/assets.md "Megascans Trees
from Unreal".

## How work runs

Since 2026-11-27 the director session drives up to four lane windows by
cross-session messages (no pasting between windows); at most two are
GPU-heavy at once, the others do CPU work (ports, reviews, docs, cooks).
GPU turns: one lock, not FIFO; release at most two lanes at a time, in
turns; a lane takes the lock per part of at most 5 launches and folds its
views into `--capture-views`. Verify every hash a lane quotes with git.
Lanes are started from `.superpowers/fable-review/prompts/*.md` (main
checkout, git-ignored) and report by a report file plus a message. Reviews
go to a lane window that did not write the code. Never Fable.

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
- No frame-time limit while a look is being found; a general performance
  pass comes later; costs are still reported (user, 2026-11-26).
- Stability over sharpness; judge clarity in motion at user speeds.
- Reports: "status" = running and waiting work only; "overview" = done.
- Sonnet for mechanical work, Opus for look judgment and risky reviews;
  never Fable.

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
