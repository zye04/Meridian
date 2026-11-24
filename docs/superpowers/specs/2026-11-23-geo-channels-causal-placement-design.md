# Geo channels and causal placement (terrain lane steps 2-3)

Status: implemented (2026-11-24); results in decisions.md "Geo channels and causal placement".

Context: `research/2026-11-23-terrain-triage.md` (direction, decisions 2-3,
plan steps 2-3; step 1 result: real relief matters, the patch at
`assets/terrain/relief-0.5.height` is the test area),
`research/2026-11-23-terrain-pcg-deep-research.md` section 2 (channel
formulas and sources).

## Intent

Walking the game, placement looks like decoration: objects sit where
noise put them, not where they would grow or fall. Real untouched nature
looks "odd yet right" (trees clinging to ravine walls, rocks gathered under
cliffs, ferns in damp hollows) because every object has a cause in the
ground. This work gives the ground those causes as data (geo channels,
computed once in the cook from any heightfield) and rewrites the placement
layers to read them, with layers that depend on earlier layers (saplings
and ferns where the forest is).

Today: each `PlacementLayer`'s `DensityRule` is slope x altitude x one
shared fractal mask x per-layer cluster noise, evaluated at the nearest
heightfield sample with integer math (`densityCore`, placement.cpp, and its
twin placement.glsl), bit-exact on CPU and GPU. Forests are mask blobs on
slopes under 42 degrees; the terrain material's forest floor runs the same
mask (`forestMask`, surface.glsl).

## Goals

1. **Geo channels in the cook.** `meridian-cook --geo in.height out.geo
   SPACING [LATITUDE]` computes, from any heightfield, a grid of 8-bit
   channels at its own spacing, centred like the heightfield:
   - 0 wetness: topographic wetness index ln(a / tan b) (Beven and Kirkby
     1979), a from multiple-flow-direction accumulation (Freeman 1991 FD8,
     or MD-infinity; not D8, which stripes at 45 degrees) on a
     depression-filled DEM (priority-flood, Barnes et al. 2014) smoothed to
     ~4 m (Lidberg et al. 2021), slope b at full resolution.
   - 1 flow: log2 of the specific catchment area (gullies, washed ground).
   - 2 local position: TPI at r ~ 6 m (h minus the mean around it; hummocks
     vs hollows), signed around 128.
   - 3 landform position: TPI at r ~ 40 m (ridges, spurs vs valleys,
     ravines), signed around 128.
   - 4 sun exposure: annual mean direct sun on the surface at the given
     latitude (default 47 N), with horizon shading by the terrain (horizon
     angles in 16 azimuths, as the shadow-height map's march).
   - 5 talus: rockfall runout below steep sources: a cell is in reach of a
     source (slope over ~40 degrees) above it along descent when the line
     from the source stays above the energy line (Fahrboschung, ~32
     degrees; Heim 1932); value = closeness to the source's foot.
   - 6, 7 reserved (0).
   Each channel's scale (what 0 and 255 mean) is fixed in the cook and
   documented in one header; the engine never interprets it.
2. **Engine contract: generic channels.** `loadGeoChannels` reads the grid;
   `Renderer::setGeoChannels` uploads it (two RGBA8 images or one buffer;
   builder's call). A `DensityRule` gains up to 4 channel ramps (channel
   index, lo, hi; lo > hi inverts, as the mask) multiplied into the
   density with the existing `ramp16`/`smooth16`, read at the nearest
   channel texel with integer math on CPU and GPU. No channel names in the
   engine.
3. **Layer dependence.** A `DensityRule` may name an earlier layer and a
   ramp over that layer's density at the same point (a pure function, so
   still bit-exact and nothing stored). One level deep (a parent may not
   itself have a parent) unless the plan finds deeper is needed.
4. **Causal layers in the game.** `placeScenery` rewritten on the channels:
   forest (landform position, slope, sun exposure, wetness), lone trees
   (open ground near forest edges and on spurs), saplings (forest density
   band), ferns (forest density + wetness + low sun), rocks (talus +
   steep), meadow plants by wetness and exposure. Rules stay few and
   physical; tuning is the builder's, judged by the score and captures.
   The terrain material's forest floor follows the forest layer's density
   (replacing `forestMask`), so floor and trees agree by construction.
5. **Forest ground truth.** `tools/fetch-relief-patch.sh` also fetches
   swissSURFACE3D Raster 0.5 m for the patch (same tiles; free, "©
   swisstopo"; licence to confirm at fetch) and writes a canopy height
   model (surface minus ground). Truth forest per 8 m cell: canopy over 3 m
   covers at least 20% (close to the Swiss NFI definition). A game option
   `--score-forest TRUTH` evaluates the forest layer's density on the CPU
   reference (`densityAt`) over the same cells and prints precision,
   recall and F1 of density >= 0.5 vs truth, then exits.

## Settled details (builder check, 2026-11-23; "call" = Claude's, for the user to check)

- **Score vs look (call).** The look wins. The F1 targets are recorded
  goals, not merge gates. Rule tuning stops after two rounds that each
  gain under 0.02 F1. The patch (E 2637..2641, N 1203..1207) lies inside
  the Napf crop and holds forested ravines and open ridges, so rules tuned
  there carry over to the Napf.
- **Channel addressing (call).** The channel spacing is an integer
  multiple r of the heightfield spacing, and the grid covers exactly the
  same samples: width = r x channel width (8000 = 4 x 2000; 7500 = 2 x
  3750). The cook refuses anything else. A point's channel texel is its
  heightfield sample index / r (integer). Outside the crop, indices mirror
  like the heightfield. The `.geo` header repeats the heightfield's size
  and spacing, and the game refuses a mismatched pair.
- **Existing rule fields stay (call).** Slope, altitude, mask and cluster
  stay in `DensityRule` (additive change). Forest extent must come from
  channels. Noise may only vary density inside it (clusters, gaps), never
  decide where forests are. Lone trees take the forest layer as their
  parent with an inverted ramp (open ground near forest), replacing the
  mask complement.
- **Parent density (call).** The parent's raw rule density at the same
  point, without streamed thinning or fade. The parent must come earlier
  in the layer list. Static and streamed are both allowed either way (it
  is a pure function, nothing is read from instances). The far-field
  splat reads placed instances, so it is unaffected.
- **Forest floor (call).** `surface.glsl` evaluates the forest layer's
  density through the same GLSL rule code as placement (one shared include
  for rules and channels). To avoid 2-4 m blocks at eye height, it
  evaluates the four surrounding channel texels' densities and blends them
  bilinearly. This is a float blend in the material only; placement stays
  integer. Today's floor ramp bands apply to that density. Outside the
  crop there is no forest floor, as today. The scene buffer gains the
  forest rule (and its parent, if any) for this.
- **The forest layer (call).** Layer id 1. Its `DensityRule` is built by
  one function that `placeScenery` and `--score-forest` share, so the
  score runs without loading tree assets.
- **Memory (call).** 112 MB for the Napf's channels on the GPU is
  accepted (16 GB card; read at load in ~0.1 s). Revisit with packing or
  8 m only if startup or memory budgets say so.

## Non-goals

- Instance-level dependence (plants in the lee of a specific rock): needs
  a load-time lookup of placed instances; only if goal 3 visibly falls
  short.
- Soil depth / sediment from an erosion simulation; strata.
- Material changes beyond the forest floor (wet soil darkening, scree
  material from talus): the channels make them possible later.
- The height clipmap for 0.5 m over 15 km; runtime weather.
- New assets.

## Anti-goals (failure even if it works)

- Channel semantics in the engine (asset/world knowledge belongs to cook
  and game).
- Any CPU/GPU placement mismatch (`placement check`, `stream check` must
  stay 0).
- Copying reality: using the truth map as an input, or so many tuned rules
  that the score is fitted rather than explained. A rule needs a physical
  reason in a one-line comment.
- Winning the score with noise masks.
- Startup regression over ~1 s in Release, or a frame-time regression on
  the parity views.

## Constraints

- Placement keep/reject stays integer-only (no floats on that path).
- The engine stays asset-agnostic; channels are generic data.
- The Napf (15 km, 2 m) and the patch (4 km, 0.5 m) both get `.geo`
  files: channels at 2 m on the patch (2000^2 x 8 B = 32 MB), 4 m on the
  Napf (3750^2 x 8 B = 112 MB). The cook computes at the heightfield's
  resolution and reduces to the channel spacing.
- The Napf's real forest edges are partly land use (pasture cleared on
  gentle ground, forest left on steep ground). Rules may encode that
  logic (it is how cultivated landscapes work), not individual edges.
- Placement hashes in CLAUDE.md change; record the new ones.

## Approaches considered

Computing the channels:
- **Own C++ in meridian-cook (chosen).** Textbook algorithms, ~400-600
  lines, unit-tested on analytic surfaces (plane, cone, V-valley, bowl);
  no external tools; runs on any heightfield, including generated ones.
- WhiteboxTools via a script: proven algorithms but a pinned external
  binary, GeoTIFF round trips, and the pipeline outside our tests. Kept as
  a fallback reference if our results look wrong.

Layer dependence:
- **Density of an earlier layer (chosen).** Pure function, bit-exact,
  nothing stored.
- Instance lookups at load: more realistic for small objects, more
  machinery; deferred (non-goal).

Role of the real forest data (user, 2026-11-23): **ground truth only**
(score the rules), not an input (would only work where data exists and
teach nothing), not ignored.

## Success criteria

1. Unit tests per channel on analytic surfaces (flow converges in a
   V-valley and spreads on a cone; TPI sign on ridge and bowl; sun exposure
   higher on a south slope than a north one at 47 N; talus below a step,
   none above it); CPU vs GPU channel reads in the Debug placement check.
2. `placement check` and `stream check` 0 mismatches on the patch and the
   Napf, with and without `--trees`.
3. Forest score: baseline (today's mask rule) measured first (2026-11-23,
   relief patch: precision 0.546, recall 0.320, F1 0.403, truth forest
   69.4%); the causal
   forest at least 0.2 F1 above it and at least 0.65 F1 (targets to
   revisit once the truth is measured; record both numbers).
4. Captures on the patch for the user to judge (forest overview from
   above, eye-height forest edge, a ravine wall, a scree foot, a wet
   hollow), causal vs today's rules.
5. Release startup within ~1 s of today's; parity route frame times
   unchanged within run-to-run noise.

## Left to the builder

- The channel storage layout on the GPU and how `PlacementConstants` /
  `SceneData` reach it; the 8-bit scales per channel (documented).
- FD8 vs MD-infinity; the smoothing filter; horizon march details.
- The exact rules and their constants (with one-line physical reasons).
- How `--score-forest` maps truth cells onto the heightfield grid.
- Task split and order (suggested: cook channels + tests, truth + score
  with today's rules as baseline, engine channel ramps, layer dependence,
  layer rewrite + material forest floor, Napf `.geo` + docs).
