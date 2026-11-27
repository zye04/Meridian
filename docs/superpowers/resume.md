# Resume here (entry point; updated 2026-11-27 night)

Read this file first, ask the open questions below, and update the file
before ending the session. It holds current truth only: open questions,
branches, the order of work, the gates, and the state of main. Session
history lives in git (`git log -- docs/superpowers/resume.md`) and in
`docs/decisions.md`. Known issues: `docs/backlog.md`.

## Open questions for the user

At most three, each answered in chat (the user opens no documents); the
agent writes the answer into the spec or here. Images are in the main
checkout under `.superpowers/fable-review/img/` (git-ignored).

1. Crown colour: greens x1, no hue shift, white balance 6504 K gives the
   reference's crown colour at o4 (top 5 % 0.529 / 0.035 / 133 against
   1.jpg 0.527 / 0.033 / 133) but dulls the meadow (chroma 0.078 -> 0.049,
   reference 0.086). Keep the working look or take the neutral greens?
   Source: `../meridian-light/.superpowers/light/REPORT-r5.md`.
2. Trees (reopened): the evidence says clumped crowns (sub-crown masses),
   not denser ones (`../meridian-trees/.superpowers/trees/dense/REPORT.md`).
   Free one-tree premise test, buy a pack, or leave? Re-ask after the
   path-traced targets lane. Tools must be script-driven (SpeedTree was
   rejected because the agent could not drive its GUI).

Decided today (2026-11-27): working look by eye is EV -0.85, offset 0.006,
palette light1, white balance 7500 K (`../meridian-light/.superpowers/light/grades/wb7500.json`);
leaf light B in and on by default. Parked, not for raising unprompted:
DLSS 5 NR via Optiscaler as a look reference (never a shipping path); the
Unreal comparison.

## The lighting target

Our own `--reference` path tracer, on our trees at the same camera, gives
the reference's kind of image: dark, crisp crowns over dark gaps. At the
same exposure the path tracer's crowns are about 3x darker with 2-3x the
brightness-normalised pixel contrast (backlog "Raster over-lights crown
interiors"). The raster over-lights crown interiors and gaps, so the
look gap is lighting and needs engine work. The path tracer is the
same-scene target for lighting. Open: what it does with haze and far
trees, noise at 16 spp (1.8 %), the exposure-matched comparison.

User's criterion: forms read as solid at every distance ("structure, not
busy"); measure with `tools/band-contrast.sh` plus a squint pair. Ours
is soft at pixel size (o4 2 / 6 px 5.5 / 7.3 against 10.1 / 11.2) and
equal at crown size.

## Branches (2026-11-27 night; all pushed unless noted)

| Branch | Worktree | State |
|---|---|---|
| `main` a43f627 | `../meridian-main` | the visibility buffer spec is the only merge of the day. The main checkout stays on `g1` |
| `crown-fill` 505eb84 | `../meridian-fill` | port done, NOT merged. Review: merge after fixes F1-F4 (backlog row); on costs +2.6 ms (approved 1.4-2.0). Held until compared with the path tracer: it adds light inside crowns. Report `.superpowers/fill/REPORT.md` |
| `visibility-buffer` a10ba99 | `../meridian-vb` | stage 1 and stage 2 partial behind `--visibility`. Frame ms off -> on: edge 28.78 -> 22.35, start 23.90 -> 19.99, meadow 15.37 -> 12.57, overview 27.86 -> 26.86, s1 24.49 -> 24.01; look within floor + 0.3; +30 MiB. Owed before T9: motion gates on (`.superpowers/vb/t8/gates-rest.sh`, ~1.5 h), a Debug capture with the resolve, then the midpoint review. Report `.superpowers/vb/REPORT.md` |
| `far-light` cab5d52 | `../meridian-light` | closed. Far-field rays: no effect; tone closes the black level; softness is not the upscaler; leaf light B kept (port: commit 5d03ce6 only, notes in `REPORT-r5.md`; unchecked with GI off). Reports `.superpowers/light/REPORT-r3.md`, `-r4.md`, `-r5.md`, `LEDGER.md` |
| `trees-dense` 5f9872a | `../meridian-trees` | closed. 2x leaves closes a lone crown, not the forest's read; found the voxel hidden cut (backlog). Report `.superpowers/trees/dense/REPORT.md` |
| `pt-targets`, `port-b-look` | `../meridian-light`, a port worktree | started 2026-11-27 night (path-traced targets; leaf light B and the working look to main); state not recorded here, read their `.superpowers/` reports |
| `far-tier` 1deda3b | `../meridian-far` | far tile tier per `specs/2026-11-26-far-tile-tier-design.md` (on that branch); waits for the user's verdict and the visibility buffer; residuals are backlog rows on the branch; no merge |
| `scene-broadleaf`, `crown-light`, `voxel-crown-9`, `fix-light-temporal`, `canopy-backlit`, `vb-stage0`, `trees-domes` | see Workspace | done or parked; detail in `.superpowers/fable-review/handover-2026-11-27-midday.md` |

Run-to-run floor with placement on: 0.2-0.6/255; only `--placement off`
is exact. Lane report shape: see CLAUDE.md.

## Ordered next steps (updated 2026-11-27 night)

1. Leaf light B and the working look to main (port lane), a ground
   walking view in every look sheet.
2. Path-traced targets beside it (one session, must end in one named fix):
   `--reference` at 64+ spp at o4, s1, h2 and the lone tree,
   exposure-matched; whole frames reference | raster | path traced;
   `tools/reference-compare.py` per region and per light term; what the
   tracer does with haze and far trees. Command: `REPORT-r5.md`.
3. Build the lighting fix item 2 names (likely per-pixel occlusion on the
   visibility buffer's resolve). Then re-judge crown fill and the trees'
   form under correct light.
4. Visibility buffer behind it: owed gates, midpoint review, merge stages
   1-2 (flag off by default), then stage 3 (voxels, where the aerial views
   gain). GPU runs queue on one lock: one GPU-heavy lane at a time
   while the look lanes run.
5. Crown fill fixes F1-F4 only if item 3 keeps it. Shadowed haze lane
   (`prompts/haze-lane.md`, main checkout) after item 2 reports on haze.
   Voxel hidden-cut fix after the buffer's voxel stage.
6. Trees: the user's pending choice (open question 2).
7. Later (unchanged): bark material lane; far wood triangle cost; AO
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

## State of main (a43f627)

Native 1440p; DLSS Super Resolution DLAA default (own stable TAA =
fallback, Ray Reconstruction an A/B); raster cascaded sun shadows default;
GI probes default; reference path tracer (`--reference SPP`); Megascans
Trees under `--trees`; voxel levels inside the trees' cluster DAGs (G1),
casting into sun cascades 0-3, far field from the same voxelizer; volumetric
clouds; valley-anchored haze; geo channels and causal placement;
crown-specular and foliage specular antialiasing (on by default); the
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
