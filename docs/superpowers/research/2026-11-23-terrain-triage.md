# Terrain triage: direction for the terrain lane (2026-11-23)

Status: direction agreed in discussion with the user, nothing implemented.
Input: `2026-11-23-terrain-pcg-deep-research.md` (Neon report). This note
records what was decided, why, and the first steps with their stop points.

## The problem, in the user's words

Walking in the game (eye 1.7 m), the ground does not feel real, even with
the 4K Megascans surfaces, POM, micro shadows and contact shadows. Real
untouched nature (the user's local mountain) is uneven everywhere, its
"noise" looks natural, and the placement of everything (trees on cliff
sides, rocks, clutter) looks odd yet right. Lighting and AO are the
lighting lane's; this lane is the terrain and its placement only.

## Diagnosis

The missing scale band is ~5 cm to ~3 m of ground relief, seen 2-30 m from
the eye:

- the data is a 2 m grid, bilinear: nothing under ~4 m;
- `terrain_detail.glsl` adds isotropic value noise (wavelengths 4-0.5 m,
  0.3 m amplitude, 0.5 m quads): wobble, not ground shaped by water, roots
  and gravity;
- textures, POM and micro shadows act at centimetres and never change the
  silhouette.

Placement has no causes: slope/height/noise masks over dither patterns, so
objects decorate the terrain instead of sitting where they would grow or
fall.

## Decisions

1. **Three bands, nothing stored below ~1 m.** Storage forces it: 15 km
   at 1 m is 15000^2 (~450 MB of 16-bit), 0.5 m ~1.8 GB, 0.25 m ~7 GB,
   0.1 m ~45 GB. Streaming huge static detail is what causes pop-in and
   frame dips; synthesis from resident data does not.
   - Landforms (km to ~1 m): stored heightfield + channels, made once.
   - Fine relief (~1 m to 0.1 m): a pure function of position and the
     channels, near the player (in the mesh shader or a ring texture
     refilled in strips like the streamed meadow ring). No state, same
     every visit; placement evaluates the same function.
   - Objects (0.1-1 m): stones, roots, litter, branches, moss, placed by
     rules (the existing placement system), nothing stored.
2. **Causal placement is the biggest lever.** Placement reads physical
   fields (soil depth, wetness, light, exposure, ledges, scree runs), layers
   depend on earlier layers (trees, then saplings / shade plants / litter;
   rocks, then plants in their lee), and each instance adapts to its spot
   (lean, size, stunting, burial and orientation of rocks). Rare "surreal
   but natural" spots (a tree on a cliff ledge) then emerge from the rules.
   Fields are data, so CPU/GPU placement stays bit-exact. Ecosystem
   simulation (Cordonnier et al. 2017, EcoBrush, Synthetic Silviculture;
   cited from memory, verify) only if rules visibly fall short.
3. **Engine contract: heightfield + generic channels.** The engine does not
   care whether they come from real data, a generator or Houdini; it stays
   asset-agnostic and not a "real-world importer". Terrain material and
   placement read the same channels, so they agree by construction.
4. **Landforms: real elevation data now; no own generator yet.** Real data
   is unbeatable at the landform scale (real erosion) and free. Designed
   landscapes later, from free tools: Fastscape (fastscapelib, open-source
   landscape evolution: flow routing, depression filling, channel erosion,
   hillslope diffusion; licence to check) for generated or re-eroded
   landforms, Blender for hand-sculpted landmarks (mesh-based, so only a
   few km at fine resolution or 15 km at 4-8 m; weak erosion). Houdini
   Indie optional, much later, if an artist GUI is wanted.
5. **Rejected or parked from the Neon report:** runtime erosion (breaks the
   shared heightfield and determinism); a surface clipmap only if measuring
   the terrain pass shows it is needed (the report's ms figures are
   estimates); heightfield streaming only if stored 0.5 m detail proves
   required; the runtime weather layer (wet, snow, season) after the
   wetness channel exists.

## Tool licensing facts (checked 2026-11-23)

- Houdini Apprentice: non-commercial, all output capped at 1280x720 and
  watermarked, heightfields included (SideFX forum). Learning only.
- Gaea 2 Community: non-commercial, export up to 1024^2; Indie $99 (8K),
  Professional $199 (16K, tiled larger). Believed Windows-only: check, the
  user runs Linux.
- World Machine Basic: non-commercial, 513^2 output; Indie $119. Believed
  Windows-only.
- swissALTI3D is published at 0.5 m and 2 m, free for commercial use with
  credit ("© swisstopo").

Free path: real data + WhiteboxTools (geo-maps) + Fastscape + Blender,
driven by scripts. Paid tools buy convenience, not fidelity; the near-player
realism is engine work on every route.

## Plan (each step has a stop point)

1. **Relief test (~half a session).** Download the swissALTI3D 0.5 m tiles
   for a 3.75 km square inside the current Napf area, cook with
   `meridian-cook --heightfield ... 0.5` (7500^2, today's file size), detail
   noise off, `--streamed off` (the meadow is being reworked and hides the
   ground). Capture the same eye-height views on open ground against the
   2 m terrain. Caveats: some quadtree constants assume 2 m data; lidar
   under forest is partly interpolated. Stop: if 0.5 m is not clearly
   better, relief is not the gap; spend the effort on placement and
   material instead. The patch stays as reference data either way.
2. **Geo-maps in the cook.** WhiteboxTools from any heightfield:
   multi-direction flow accumulation on a pit-breached DEM (not D8), wetness
   index, curvature (profile, plan), exposure/insolation, scree; engine
   input = generic heightfield + channels.
3. **Causal placement + a clutter layer** on those channels (decision 2).
4. **Fine relief synthesis** in the `terrain_detail.glsl` slot, only if
   step 1 showed relief matters; judged against the real 0.5 m patch
   (synthesize 2 m -> 0.5 m, compare). Shapes driven by the channels (wet
   hollows, scree steps, tussocks, root mounds, terracettes), statistics
   learned from real 0.5 m lidar. Least proven part.
5. **Landforms:** keep Napf or pick a crop closer to the Chlorophyll frames
   (river valley, cliff bands, forested slopes). Fastscape / Blender only
   when designed landscapes are wanted.

Also outside this lane but needed for the look: ground material transitions
(rock into soil into grass), asset density up close, and lighting
(AO / small-scale shadowing, the lighting lane).

## Asked of the user

Eye-height photos of the ground on the local mountain (open ground, forest
floor, slopes, path edges, several distances, soft and direct sun): the
close-range reference for this lane, as the Chlorophyll frames are for the
overall look.

## Step 1 result: relief test (2026-11-23)

Patch: swissALTI3D E 2637..2640, N 1203..1206 (16 tiles, 2025 and 2022
surveys, each 0.5 m tile with the 2 m tile of the same year),
`tools/fetch-relief-patch.sh` -> `relief-0.5.height` (8000^2) and
`relief-2.height` (2000^2), same area and centre. Checks: heights at the
start, summit and two other points agree with `napf.height` within 1 cm
(0.5 m within 0.6 m on a steep slope); the 2 m grid is the exact 4x4 box
mean of the 0.5 m one (mean difference 0.000 m); no steps at tile edges.
The engine needed nothing for 0.5 m spacing (level 0's 0.5 m quads land on
samples); new are `--terrain FILE` and `--effects terrain-detail=0`.

Statistics: the 0.5 m data minus the bilinear 2 m surface, std per 32 m
block: median 0.115 m, p90 0.21 m, p99 0.85 m, max 3.7 m. Blocks above
~0.8 m are all slopes over 40 degrees (the Napf's Nagelfluh ravines);
pasture carries ~0.1 m.

Captures (`docs/images/relief-test-2026-11-23/`, rows: 2 m + detail noise
(today), 2 m bare, 0.5 m bare; `--placement off`; midday and an 8 degree
sun from the side; views in `views.txt`: name, camera on 0.5 m, camera y on
2 m, low sun):

- Low sun on a 21 degree pasture slope (`slope-low.jpg`) decides it: the
  detail noise is an isotropic blob field that reads as artificial; 2 m
  bare is flat; 0.5 m shows soft undulations along the contours and,
  50-500 m out, gullies, a path cut and small slumps that 2 m blurs.
- Ravine (51 degrees, `ravine-mid.jpg`): ledges and a stepped hollow at
  0.5 m, mush at 2 m.
- Ridge top and valley floor at midday: 0.5 m ~ 2 m bare; the detail
  noise adds bumps the real ground does not have.

Verdict: relief matters, and its character (anisotropic, slope-aligned,
sparse on pasture, strong on steep ground) matters more than its
amplitude. Today's isotropic noise is worse than none on open pasture. The
gain also reaches mid-distance hillsides, where the detail noise never
acts. Caveat: the shadow-height map's 4 m texels cast no shadows of the
0.5 m relief; what shows is normals, silhouettes and contact shadows.

## Steps 2-3 result (2026-11-24)

Done: geo channels in the cook and causal placement on them (decisions.md
"Geo channels and causal placement"). The forest follows the Napf's land
use (steep and shaded ground wooded, gentle sunny ground pasture), scored
against swissSURFACE3D canopy at kappa 0.461. Next for the lane: step 4
(fine relief synthesis, judged against the 0.5 m patch) or the 0.5 m
height clipmap when the showcase needs the full area; instance-level
dependence and wet/scree materials are in the backlog.
