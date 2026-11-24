# G7: grey, washed-out image in forests (diagnosis, 2026-11-23)

Issue #7, plan `../archive/plans/2026-11-21-phase0-trustworthy-judgment.md` Task 4.
Release, DLSS DLAA, 2560x1440 fullscreen (headless 1440p output), `--trees`
with the Megascans Trees (27 forest, 10 lone, 18 sapling prototypes),
`--frames 120`, 0 validation errors in every run. Captures are git-ignored,
in the g7 worktree's `.superpowers/g7/cap/` (`NAME.pfm` = frame 120,
`NAME_000.png` = frame 119 of the same run); side-by-sides in
`.superpowers/g7/sbs/`.

## Views

| Name | Camera | What |
|---|---|---|
| interior | `85 1158.6 -53 -1.55 0.25` | forest floor under the canopy, sunflecks |
| edge | `600 1205.6 -400 -1.55 0.02` | inside the forest edge, bright meadow behind |
| valley | `1119 1355.9 -431 -1.55 -0.1` | the open start view (control) |

The user's screenshots (`user-02`, `user-05`) were taken interactively with
the grade file; `user-02` is the interior/edge situation. `user-05` (warm,
hazy valley) was not matched; the valley control is the open reference.

Measures: EV100 from the capture log; OKLab L mean / 5th percentile of the
display PNG (whole frame); absolute luminance from the `.pfm` (pre-exposed,
times 1.2 * 2^EV100), floor rect `0 100 1600 1000`.
Targets: the Chlorophyll frames (`assets/reference/chlorophyll/`, bars
cropped) have whole-frame mean L 0.53-0.56 and p5 0.15-0.24 (all open
scenes, no forest interior among them); closed summer beech canopy PAR is
1.5-5% of open light, ~12% 32 m in from the edge, 45-55% at the edge
(neon triage, #7).

## One factor at a time (before the fix)

| Run | EV100 | mean L | p5 L | floor p50 (abs) |
|---|---|---|---|---|
| valley (open) | 13.56 | 0.565 | 0.125 | - |
| interior | 10.21 | 0.564 | 0.427 | 208 |
| interior `--haze clean` | 9.94 | 0.565 | 0.426 | 170 |
| interior `--effects micro=0` | 10.37 | 0.575 | 0.428 | 208 |
| interior `--gi probes` | 8.01 | 0.539 | 0.376 | 41.7 |
| edge | 9.57 | 0.517 | 0.182 | - |
| edge `--haze clean` | 9.55 | 0.489 | 0.153 | - |
| edge `--effects micro=0` | 11.24 | 0.577 | 0.362 | - |
| edge `--gi probes` | 9.41 | 0.354 | 0.084 | - |

The grade file (`~/.config/meridian/grade.json`) is the built-in look: same
sun (51.3 / 40.7 deg), same grade; it differs only by cloud outline 0
(valley diff 0.6/255, all in the clouds). Not a factor. The Megaplants vs
Megascans A/B was skipped: the Megascans Trees are the default now and
leaf colour is G6/G15, not exposure.

Sunflecks on the interior floor (same soil, p99-p99.9 of the rect) are
1500-2100; the shade p50 is 208, so shade is 10-14% of sunlit soil without
GI, and ~2.5% with `--gi probes` (41.7).

## Cause

1. **No canopy occlusion of the sky ambient without GI.** The terrain under
   the trees is lit by the unoccluded sky irradiance (blue), so the forest
   floor sits at 10-14% of open light (target 1.5-5%) with the sky's
   blue-grey cast. `--gi probes` traces the canopy (far-field voxels) and
   puts it at ~2.5%, green-tinted by the leaves.
2. **Mean-to-grey metering lifts every forest back to mid-grey.** The
   forest views meter 3.4-4 EV above the open view (5.5 EV with GI), so
   whatever the lighting, the frame comes out at L ~0.55 with no darks (p5
   0.43 against 0.15-0.24) and, with GI, blown sunflecks.

Haze (-0.27 EV, L unchanged) and the grade file do not contribute. Micro
occlusion matters for the trees, not the floor: `micro=0` also drops the
crown sky visibility (CLAUDE.md, plant micro occlusion), and the edge's
trunks and leaves turn grey-white (`sbs/edge-base-vs-micro0.jpg`), the look
of `user-02`. Without GI, crown visibility is the only canopy occlusion of
the ambient: trees get it, the terrain under them gets none.

## Look decision (user, 2026-11-23)

Emulated lift caps (grade `exposureEv`, which scales before the tonemap and
not the meter) on the GI interior: 4 EV below the open meter -> L 0.321 /
p5 0.181; 3 EV -> 0.207 / 0.105 (`sbs/interior-grid.jpg`: today, GI, GI +
4 EV, GI + 3 EV). The user picked 3 EV as the most photoreal baseline,
noting the trunks read very dark; readable shade is a separate feature
(local exposure, below).

## Fix: shade lift floor in exposure.comp

`sky_irradiance.comp` also writes `openIlluminance`: lux on open, upward
ground at the camera (sun through the atmosphere and the cloud shadow, plus
the sky irradiance with clouds). `exposure.comp` never lets the target rise
more than `kMaxShadeLiftEv` = 3 above the EV of an open landscape of
reflectance `kOpenReflectance` = 0.09 under that light (with 0.09 the
start view's own meter, 13.56, and the model, 13.39, agree; 0.18 put the
floor a full EV darker than the chosen look). Open views are unchanged (the
floor sits ~3 EV below their meter); overcast and cloud shadow lower the
floor with the light; at night it never binds.

### After the fix

| Run | EV100 before -> after | mean L | p5 L |
|---|---|---|---|
| interior (GI off, the default) | 10.21 -> 10.60 | 0.564 -> 0.505 | 0.427 -> 0.368 |
| edge (GI off) | 9.57 -> 10.60 | 0.517 -> 0.394 | 0.182 -> 0.101 |
| interior `--gi probes` | 8.01 -> 10.60 | 0.539 -> 0.205 | 0.376 -> 0.102 |
| edge `--gi probes` | 9.41 -> 10.60 | 0.354 -> 0.256 | 0.084 -> 0.000 |
| valley (open) | 13.56 -> 13.56 | 0.565 | 0.125 |

Side-by-sides: `sbs/fix-before-after.jpg` (interior and edge, GI off),
`sbs/edge-gi-before-after.jpg`. With GI off the floor stays blue-grey: the
exposure no longer hides it, but cause 1 needs the canopy (#29) or GI.

Motion gates: unchanged by construction. The gate views (ground still,
fwd, side, pitch; beech still) meter EV100 13.70-14.39 under ~81,400 lux of
open light, where the floor is 10.39: 3.3-4.0 EV below, so `max()` returns
the old target and the exposure, hence every frame, is bit-identical. The
sky irradiance buffer's new field sits in the old pad slot, which nothing
read. GPU: the floor costs nothing measurable (interior 9.73 -> 9.71 ms,
edge 26.59 -> 26.31 ms, run-to-run noise).

GI probes cost (same views, same Render 2560x1440 line): interior 9.73 ->
11.46 ms, edge 26.59 -> 28.75 ms.

## Follow-ups (sub-issues of #7)

- #28 G7a local exposure: readable shade (trunks) under the 3 EV floor.
- #29 G7b forest floor sky ambient without GI (canopy visibility map from
  the far-field voxels; moot if GI probes become the default).
- #30 G7c forest-edge foliage near-black under GI probes (crown sun
  transmission from the prototype voxels).

The deep-research report on pure-raster forest lighting and its triage
(`2026-11-23-raster-lighting-{deep-research,triage}.md`) back each of them.
