# 15 km terrain (showcase milestone M1) — design

**Status:** done on main (M1, 4a6cc04..aa5dbe3).

## Intent

First milestone toward the 15 x 15 km natural showcase scene (forests,
meadows, hills, a peak, clouds; reference `assets/reference/target-meadow.png`).
M1 replaces the single 2 km procedural mesh with a 15 km heightfield
terrain that renders with continuous LOD, casts and receives sun shadows
at any distance, stays sharp and stable 7.5 km from the origin, and can be
walked and flown over. Every later milestone (GPU placement, grass,
clouds) builds on the heightfield, its masks and its precision rules, so
they are engine framework and must be done properly. The terrain *shape*
is content: it comes from real elevation data, not from hand-tuned noise.

Milestone order agreed with the user on 2026-11-19: M1 terrain, M2 forests
+ rocks + sky occlusion, M3 meadow grass + flowers + wind, M4 clouds, M5
performance and frame pacing; each milestone carries its own GPU budget.

## Goals

- A 15 x 15 km heightfield at 2 m spacing from real elevation data
  (swissALTI3D, the Napf area in the Emmental: forested hills, meadows,
  1406 m summit, no lakes), fetched by script, cooked once, resident on
  the GPU. The procedural generator stays as a fallback when the data is
  missing.
- Continuous LOD terrain (CDLOD), selected on the GPU, no cracks, no
  visible popping (geomorphing, with motion vectors that include it).
- Two finer levels than the data (1 m, 0.5 m) near the camera with
  procedural detail displacement.
- Terrain blocks sun-shadow rays from every surface at every distance and
  matches the raster terrain (heightfield ray march, below).
- Camera-relative rendering: geometry and terrain textures at the map
  corners are as stable (no vertex or texel jitter) as at the center.
- Terrain beyond the crop continues to the true horizon (~130 km from the
  summit) and follows the planet's curvature, so it meets the spherical
  atmosphere without a step or gap (placeholder content, below).
- Walk mode: the camera follows the ground at eye height; fly mode stays.
- Snow line becomes a parameter (it is hard-coded at 150-190 m today,
  which would cover the whole Napf, 600-1400 m, in snow).
- Budget at 2560x1440, Release, RTX 4080, median of 5: CDLOD selection +
  terrain raster <= 0.3 ms on top of the current terrain material's cost
  (the material is unchanged and out of scope); the shadow march <= 0.6 ms
  over all pixels in the open view. Capture runs report per-pass GPU
  times (timestamps around each pass), which M5 needs anyway. A missed
  budget does not block M1 by itself: stop and report the numbers to the
  user. Levers in order: march quality/length, then LOD density; not the
  material.

## Non-goals (later milestones or deferred)

- Placement of trees, rocks, grass (M2, M3); the existing `--scatter`
  keeps working on the new ground for testing.
- Sky occlusion (M2), clouds (M4).
- New terrain layers (grass for meadows comes with M3), rock hex-tiling,
  any material polish.
- Disk streaming of terrain: the heightfield is resident (112 MB as R16).
- Terrain in the TLAS; hardware RT of terrain. Rejected for M1 (below).
- Procedural erosion.
- Water.

## Anti-goals

- Any blur, shimmer or jitter in motion that the current terrain doesn't
  have (the user's no-smearing rule), including at the map corners.
- Frame-time spikes when the camera crosses LOD or tile boundaries.
- Per-frame CPU work that grows with terrain size.

## Constraints

- RTX 4080, Vulkan 1.3 + `VK_EXT_mesh_shader` (max 256 output
  vertices/primitives, 128 invocations per workgroup).
- Conventions in CLAUDE.md: meters, y = elevation above sea level,
  right-handed, Y up, reverse-Z, motion = previous - current in UV.
- Debug runs with ASan/UBSan and validation must stay usable (startup
  seconds, not minutes).
- No new third-party dependencies (`magick`, `curl`, `jq` are already used
  by the tooling).

## Approaches

**Terrain shadows (user picked A on 2026-11-19).**

- **A. Heightfield ray march (chosen).** Shadow rays query the TLAS for
  objects as now, then march a max-height mip pyramid of the heightfield
  for terrain. Exact match with the raster terrain at any distance, no
  terrain BLAS so nothing to rebuild as the camera moves, and the same
  march serves M2's sky-visibility rays. Costs ~30-50 fetches per ray
  (estimated 0.3-0.6 ms at 1440p, to measure). This is how Lumen traces
  landscapes in software.
- B. Terrain tile BLAS by LOD + partitioned TLAS. General hardware path
  (future reflections/GI get terrain for free), but tiles rebuild as the
  camera moves, costs BLAS memory and code, and needs the NV-only
  partitioned TLAS now. The partitioned TLAS comes with M2 for forests;
  terrain could move to it later if secondary rays need it.

**Terrain mesh (Claude's call).**

- **CDLOD with GPU quadtree selection (chosen).** Fixed grid per node,
  node size doubling per level, distance-based LOD ranges with
  geomorphing (Strugar 2009); selection in compute, one dispatch per
  quadtree level (Far Cry 5's GPU terrain, GDC 2018). Crack-free by
  construction, GPU-driven, tiny CPU cost.
- Geometry clipmaps: rings around the camera, no adaptivity to terrain
  detail, and the ring update logic doesn't help a GPU-driven pipeline.
- Terrain through the cluster LOD path: a 15 km heightfield at 2 m is
  ~112M triangles; building and storing its cluster DAG costs minutes and
  gigabytes for no gain over a regular grid.

**Terrain source (user picked real data on 2026-11-19).** Real elevation
data gives real erosion and ridge shapes immediately. Procedural noise +
GPU erosion was the alternative: self-contained but more code and worse
shapes. The existing value-noise generator stays as the fallback.

**Resolution (user picked 2 m on 2026-11-19).** 2 m matches the
swissALTI3D product and is 112 MB as R16; 1 m would be 4x the memory for
detail the procedural levels can stand in for at this stage.

**Camera-relative rendering (Claude's call).** Shaders compute
`world - cameraPosition` first and project with a view-projection that has
no translation. Subtracting two nearby floats is exact (Sterbenz), so the
only error left is each vertex's own world-space rounding (~0.5 mm at
7.5 km), which is static and does not jitter from frame to frame. The
instance format stays as it is. Tile + offset coordinates (UE large world
coordinates) were the alternative; they buy precision far beyond 15 km
that this world doesn't need.

## Design

### Data and tooling

- `tools/fetch-sample-assets.sh` downloads the 15 x 15 swissALTI3D 2 m
  tiles (1 km GeoTIFFs, 500 x 500 float32, LZW; newest year per tile via
  the STAC API `data.geo.admin.ch/api/stac/v0.9/collections/
  ch.swisstopo.swissalti3d`) covering LV95 E 2631-2646 km, N 1198-1213 km
  (the Napf summit tile is 2638-1205), fails loudly if any is missing,
  mosaics them with `magick` into one float32 raw image (7500 x 7500), and
  cooks it with `meridian-cook --heightfield`. Data is swisstopo open
  government data (free use, attribution "© swisstopo"); `assets/` stays
  git-ignored and the attribution goes in the script and in CLAUDE.md.
- `.height` file (cooked): small header (magic, version, width, height,
  spacing in meters, min height, height scale) + row-major uint16 samples.
  Rows run north to south. Height = min + sample * scale.
- World mapping: crop center = world origin (x, z); east = +x, north = -z;
  y = elevation in meters above sea level (Napf crop spans roughly
  600-1406 m).

### Engine API (`terrain.h`)

- `Heightfield`: width, height, spacing, min/scale, uint16 samples;
  `heightAt(x, z)` (bilinear, world meters, same sampling and outside-crop
  rule as the GPU), used by walk mode and `--scatter`.
- `loadHeightfield(path)`; `generateHeightfield(settings)` (the existing
  value noise, producing a `Heightfield`; fallback and unit tests).
- `Renderer::setTerrain(const Heightfield &)` uploads the heightfield (R16
  UNORM, scale/offset in `GpuTerrain`) and builds its max-height mip
  pyramid on the GPU. The terrain is no longer a mesh or an instance;
  `generateTerrain` and the 2 km mesh retire.
- `TerrainMaterial` gains `snowLine` (meters; default high enough that the
  Napf has no snow); the shader's hard-coded 150-190 m band uses it.

### Rendering

- **Quadtree.** Leaf nodes are 16 m at 0.5 m spacing (32 x 32 quads); each
  level doubles node size; levels 0-1 are finer than the data and add
  procedural detail displacement (amplitude a few decimeters, faded out
  before level 2 so the data level is never altered); level 2 matches the
  2 m data. The root covers the crop plus the outer placeholder area.
  Per-node height bounds come from a min/max mip of the heightfield
  (each frame, the min is lowered by the curvature drop at the node's
  farthest point from the camera and the max by the drop at its nearest;
  see Curvature below).
- **Selection (compute).** One dispatch per level, top down, with
  indirect arguments: frustum-test the node's bounds, subdivide while the
  camera is inside the node's LOD range, otherwise emit the node (with
  its level) to the terrain draw list. Standard CDLOD ranges and morph
  zones; ranges chosen so a quad stays a few pixels across at 1440p
  (builder tunes and records the numbers).
- **Raster.** A terrain mesh shader draws each emitted node as 16
  workgroups of 8 x 8 quads (81 vertices, 128 triangles), samples the
  heightfield, geomorphs vertices toward the next coarser grid in the
  morph zone, and outputs camera-relative positions. Motion vectors use
  the previous frame's camera for both the projection and the morph
  factor, so morphing doesn't smear under TAA. Normals are per pixel from
  heightfield differences in the fragment shader (vertex normals would
  change with LOD). The terrain shades through the existing terrain
  material path (a mesh.frag variant), unchanged apart from the inputs.
- **Order.** Terrain draws first in the early scene pass, frustum-culled
  only, so hills feed the Hi-Z pyramid and occlude objects behind them.
- **Outside the crop.** The heightfield is sampled with mirrored repeat,
  so hills continue without seams; aerial perspective hides the
  repetition. The quadtree root is level 16 (~524 km half-size), covering the
  horizon up to ~20 km altitude; coarse levels cost almost nothing.
  Placement (M2+) stays inside the crop. Walking past the edge is allowed.
- **Curvature (user picked on 2026-11-19): a display warp.** The world
  stays flat; only rasterized positions bend. Every rasterized vertex
  (terrain, clusters, blended) drops by d^2 / (2R) relative to the camera
  (d = horizontal distance from the camera, R = the Atmosphere's
  bottomRadius in meters), for current and previous positions (each with
  its own camera), so motion vectors stay right. Far terrain sinks below
  the horizon on its own and meets the atmosphere's spherical ground;
  objects stay on the ground they stand on at any distance. Fragments
  undo the drop to get their flat world position for lighting, shadow
  rays and the march (all in the flat world, where the TLAS lives); the
  aerial perspective uses the warped, displayed position. Near the camera
  the drop is ~0, so `heightAt` (walking, scatter) is unaffected. Culling
  bounds account for the drop (instances: lower the sphere center by the
  drop at its nearest point; terrain nodes: above).

### Shadows

- `sunVisibility` keeps its TLAS ray query (objects) and adds a march of
  the max-height pyramid along the same ray (terrain), on every surface:
  terrain, clusters, blended. It runs in the flat world (the fragment's
  unwarped position), like the TLAS query. Hierarchical: step at the
  coarsest level whose max is below the ray, descend on overlap, stop on
  a hit or at the ray length (kShadowRayLength, 20 km).
- The raster terrain at coarse LOD can lie below the bilinear heightfield
  (or above it with detail displacement). Terrain fragments start the
  march above the surface by their level's error, the same idea the
  cluster path uses (`inLodError`). No self-shadow acne at any distance is
  a requirement.
- The terrain is removed from the TLAS (it is no longer an instance).
- A brute-force linear march (compile-time define) exists for A/B checks.

### Camera-relative rendering

- `SceneData` carries a translation-free view-projection (current, jittered
  and unjittered, and previous), the camera position and the previous
  camera position. Every shader that transforms or unprojects positions
  (mesh.mesh, mesh.vert, the terrain mesh shader, sky.vert,
  aerial_perspective_lut.comp, culling) works camera-relative. Varyings
  carry camera-relative positions; world positions are rebuilt only where
  needed (shadow ray origin, atmosphere).
- Terrain texture coordinates: world-projected UVs are formed from the
  camera-relative position plus a per-layer offset that wraps the camera
  position by a large period (computed on the CPU in double), so UVs stay
  small. The hex-tiling cell hash must be periodic with that period, so
  crossing a wrap boundary changes nothing on screen.

### Game

- `main.cpp` loads `assets/terrain/napf.height` (procedural fallback with
  a printed hint when missing), starts on a meadow near the summit
  (builder picks it from the data and records it), and keeps `--scatter`
  around the start.
- Walk mode (toggle key): eye 1.7 m above `heightAt`; fly mode unchanged.
- CLAUDE.md "Useful views" are replaced by views on the new terrain:
  ground, open view toward the summit, high overview, a map corner
  (precision), sunset (long terrain shadows).

## Verification

- Unit tests: `.height` round-trip; `heightAt` bilinear and the outside-crop
  rule; max-height pyramid (CPU reference: every texel >= every sample it
  covers, and equal to the max); CDLOD range/morph math if it lives in a
  shared header; procedural fallback produces a valid heightfield.
- Captures (fewest views that prove it): ground view, summit open view,
  high overview (no cracks, no holes, LOD not visible, no edge or step
  at the horizon from the summit), sunset (terrain
  shadows from hills, compared against the brute-force march: mean
  difference within noise), map corner still vs moving at 5 m/s (motion
  sharpness ratio equal to the same test near the center within noise).
- Performance: Release, 2560x1440, median of 5 captures, GPU ms against the
  budget; report terrain nodes drawn and triangles.
- 0 validation errors, no leaks, Debug startup time noted.

## Left to the builder

Exact LOD ranges, morph zone width and detail-noise parameters; the
exact root size; whether selection is per-level dispatches or one
persistent workgroup if measurements favor it; march step details;
`.height` header layout; heightfield pyramid formats; the walk-mode key;
the wrap period for UVs.

## Sources

- Strugar 2009, Continuous Distance-Dependent Level of Detail for
  Rendering Heightmaps (CDLOD): https://github.com/fstrugar/CDLOD
- Far Cry 5 terrain (GPU quadtree, GDC 2018, Jeremy Moore)
- swissALTI3D: https://www.swisstopo.admin.ch/en/height-model-swissalti3d
  (STAC: https://data.geo.admin.ch/api/stac/v0.9/collections/ch.swisstopo.swissalti3d)
- Tevs et al. 2008, Maximum Mipmaps for Fast, Accurate, and Scalable
  Dynamic Height Field Rendering
- Unreal Lumen heightfield tracing:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
- Large worlds and camera-relative rendering:
  https://godotengine.org/article/emulating-double-precision-gpu-render-large-worlds
