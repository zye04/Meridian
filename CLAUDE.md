# Meridian

C++20 / Vulkan 1.3 game engine (`engine/`, static library `meridian`) plus
the game built on it (`game/`). Goal: photorealistic, first-person, open
world, grand scale. Hardware ray tracing is required. The setting is open,
so systems stay parametric (e.g. the atmosphere supports other planets).
Technique choices and their sources are in `docs/decisions.md`; read it
before re-researching anything.

## Working agreement

- Resuming paused work: read `docs/superpowers/resume.md` first (what is
  done, open WIP branches, next steps).

- The user steers; Claude implements, verifies, and commits/pushes to `main`.
- Commits carry **no** AI attribution trailer. One feature per commit, with
  a message that explains what changed and why.
- Before implementing a rendering/engine feature, research current (2026)
  practice and log the choice in `docs/decisions.md`.
- Priorities: photorealism, no smearing in motion, performance, clean and
  minimal code, proper game-dev practice. Keep inputs DLSS-ready (jitter,
  motion vectors, depth, exposure, G-buffer guides).
- Keep this file and `docs/decisions.md` current in the same commit as the
  change they describe.

## Commands

```bash
cmake -S . -B build && cmake --build build      # Debug: ASan+UBSan, validation
ctest --test-dir build                           # or ./build/tests/tests
flock /tmp/meridian-gpu.lock ./build/game/game   # interactive (WASD, mouse, Esc; F walk/fly)
./tools/fetch-sample-assets.sh                   # test models into assets/; also the Napf terrain (225 tiles, needs magick)
./build/tools/cook/meridian-cook in.gltf assets/Name   # PNG/JPEG -> KTX2 glTF (bakes a lone node's rotation, scale + vertical offset)
./build/tools/cook/meridian-cook --texture in.jpg out.ktx2 color|data|normal
./build/tools/cook/meridian-cook --heightfield in.f32 W H SPACING out.height   # float32 m -> 16-bit .height
cmake -S . -B build-brute -DMERIDIAN_TERRAIN_MARCH_BRUTE_FORCE=ON   # A/B: terrain shadows by a 1 m linear march
python3 -I tools/region-luminance.py in.pfm X0 Y0 X1 Y1 [...]   # mean luminance per pixel rect (top-left origin, end exclusive) + ratio to the first
```

Every run of the game binary (interactive, capture, perf; Debug or
Release) goes through `flock /tmp/meridian-gpu.lock`, because two Claude
instances may render in parallel and GPU timings would collide. Hold it
only for the run, never for builds. Examples below omit the prefix.

Controls: WASD, mouse, Esc. Fly mode: Space/Ctrl up/down, 30 m/s,
Shift x5. `F` toggles walk mode: eye 1.7 m above the ground, WASD horizontal at 1.4 m/s (Shift 5 m/s), no vertical keys.

Automated visual check (deterministic, ~4 s, exit code 1 on any Vulkan
validation error):

```bash
./build/game/game --camera X Y Z YAW PITCH [--sun X Y Z] [--scatter N] [--lod-error PX] --capture out.png [--frames N]
```

A `.pfm` capture path writes the linear HDR image the tonemap reads
(post-TAA, pre-exposed) instead, for luminance ratios.

Motion sharpness: add `--camera-velocity X Y Z` (m/s, fixed 1/60 s steps,
ending at `--camera`) and compare with the still capture of the same view:
`magick out.png -colorspace gray -morphology Convolve Laplacian:0 -format
"%[fx:standard_deviation]\n" info:` (higher = sharper).

`--haze clean|summer|summer-hazy` picks the aerosol (default summer:
Swiss summer haze kept low in the valleys, blue zenith; summer-hazy:
haze through the ~2 km boundary layer, ~45 km visibility on the Napf ridges, grey sky;
clean = Bruneton's very clean defaults, for A/B).

`--lod-error PX` sets the LOD threshold (default 1 pixel; 0 = full
detail, for A/B). Capture runs print drawn instances, clusters,
triangles and entries dropped by full cull lists (nonzero = missing
geometry), per list: chunks, work items, clusters, terrain nodes, bvh
nodes.
They also print drawn terrain nodes (2048 triangles each), `cells N`
(placement cells visible), `bvh nodes N` (cluster group BVH nodes the
traversal visited), `placed N`
(placed instances), and `GPU passes:`
with GPU ms per pass (upload + tlas, terrain select, cull cells, cull early, traverse, cull clusters, atmosphere, scene early, pyramid, cull late, scene late, taa, exposure,
tonemap; they sum to the total). A
new pass is one `endPass("name")` call in `renderer_frame.cpp`.

Useful views (Napf terrain; ground 1354.2 m at the start): the default
start is eye height on a ridge top at (1119, -431), 1.5 km east of the
Napf summit (-359, -465, 1406.7 m), looking at it past the sample models
(`1119 1355.9 -431 -1.55 -0.1`); open view toward the summit
`1119 1356 -431 -1.55 -0.02`; full-ground view `1119 1355.9 -431 -1.55
-0.25`; LOD transitions from above `1119 1420 -431 -1.55 -0.6`; high
overview (3000 m above the ground) `1119 4354 -431 -1.55 -0.35`; across
the crop's east edge into the mirrored terrain `6500 3500 0 1.57 -0.6`;
sunset, long ridge shadows across the valleys, `1119 1500 -431 3.14 -0.15
--sun 0.9 0.05 -0.4`; far horizon from the summit `-359 1408.7 -465 1.57
0`; float precision far out, at the crop's corner (ground 1286.5 m),
`7000 1288.2 7000 -1.55 -0.25` (compare its `--camera-velocity 5 0 0`
Laplacian with the still's). Placed forests (seed 1): forest edge
`600 1205.6 -400 -1.55 0.02`; forest interior `85 1158.6 -53 -1.55
0.25`; forest overview `700 1450 -400 -1.55 -0.5`;
canopy from 40 m above `85 1200 -53 -1.55 -1.2`; a
placed moss rock close up `1944.3 953.4 -1923.9 -1.55 -0.4`; trees on
the summit ridge at ~1.5 km: the open view toward the summit.
`--scatter N` places N random Poly Haven models and imported Megaplants
trees within 150 m of the start (fixed seed; loading 21 models, 14M
triangles, makes Debug startup ~2.5 min); over the scatter:
`1119 1362 -431 -1.55 -0.25`; a 27 m beech (with `--scatter 100`):
`1115.47 1366.85 -429.72 1.223 0`. Missing models drop out of the pick,
so without the Megaplants trees the same seed places different content
and these views differ.
The game places forests, lone trees and rocks over the whole Napf at load
(`placeScenery` in main.cpp; ~2.9M instances),
which loads the Megaplants trees and rocks as prototypes: Debug startup
~95 s, Release ~9.4 s. `--placement off` skips it (Debug ~18 s, Release
~2.3 s; heightfield pyramid and normal map); use it for captures that
don't need placement. Debug runs print `placement check:` (GPU table vs
the CPU reference, must be 0 mismatches) and `placement hash 0x...`
(0x1c6b673748ee8f8f for seed 1 with every asset present). Loading
placement also prints `TLAS:` (records, sizes, build ms, prototype BLAS
before and after compaction, cell margin).

## Testing policy (usage matters)

- Always build. Run tests when logic they cover changes.
- Capture only when rendered output can change, with the fewest views that
  prove the change. The capture run's output already reports validation
  errors, leaks and GPU time; don't do separate runs for those.
- GPU ms are only comparable at the same resolution. The game runs
  fullscreen on the primary monitor by default, so captures share one size
  (the 1440p monitor; Hyprland's 1920x1080 FALLBACK output when it's off).
  `--windowed` gives a tiled window, whose size varies between runs.
- Interactive runs pick up the user's mouse; use capture runs for checks.
- Captures step time by a fixed 1/60 s (motion and exposure adaptation), so
  two captures of the same view with the same build match almost exactly
  (mean difference ~1e-6/255; GPU atomics can still reorder a pixel).
  Debug and Release differ by ~0.02/255. Compare mean differences, not
  exact pixels.
- Prefer unit tests for math (e.g. frustum planes in tests/mat4_test.cpp)
  over visual checks.

## Layout

```
engine/include/meridian/   public API: renderer.h (Renderer, Lighting,
                           Atmosphere, materials), camera.h, mesh.h,
                           model.h (glTF), texture.h (KTX2), cluster.h
                           (cluster LOD DAG, LOD cut rule, group BVH),
                           terrain.h (Heightfield, height pyramid),
                           placement.h (rules, dither pattern,
                           evaluatePoint: CPU reference of GPU placement,
                           PlacedInstance), window.h, math/{vec3,mat4}.h
engine/src/render/         renderer internals:
  gpu_types.h              constants + structs mirroring GLSL layouts
  vulkan_util.*            barriers, shader loading, pipeline builders
  renderer_impl.h          Renderer::Impl + submitNow()
  renderer_setup.cpp       init/teardown: swapchain, frames, descriptors,
                           pipelines, atmosphere, materials
  renderer_resources.cpp   buffers, images, BLAS (buildBlas: one geometry
                           per mesh, compacted; a hand-placed mesh's on
                           its first addInstance), texture/material/mesh
                           upload
  renderer_terrain.cpp     setTerrain (height image, normal map, pyramid),
                           CDLOD selection dispatches, terrain draw
  renderer_placement.cpp   setPlacement: count, scan, write at load (two
                           submits), Debug check against evaluatePoint;
                           buildPlacedTlas: prototype BLAS + PrototypeInfo,
                           cell margin, TLAS fill (tlas_fill.comp) + build
  renderer_frame.cpp       per-frame: instance upload + TLAS build when
                           instances change, GPU culling, all passes in
                           draw()
engine/src/                model.cpp (glTF), texture.cpp (KTX2),
                           cluster.cpp + clusterlod.cpp (meshoptimizer),
                           terrain.cpp, placement.cpp, window.cpp
engine/shaders/            GLSL; *.glsl are includes (see below). Terrain:
                           terrain.glsl (heightfield sampling + mirror,
                           quadtree, morph, detail noise),
                           terrain_select.comp, terrain.mesh, terrain.frag
                           (mesh.frag with TERRAIN), terrain_normals.comp.
                           placement.glsl: integer keep/reject twin of
                           placement.cpp; placement.comp
                           (PLACEMENT_COUNT / PLACEMENT_WRITE variants)
                           and scan.comp (exclusive scan) run at load;
                           tlas_fill.comp writes the placed TLAS records.
                           cell_cull.comp: placement cells -> instance
                           chunks for cull.comp -> BVH seeds for
                           cluster_traverse.comp -> work items for
                           cluster_cull.comp
game/src/main.cpp          scene setup, fly camera, CLI options
tools/cook/                meridian-cook; tree.cpp (--tree: simplify,
                           prune, flatten), ktx2.cpp (shared encoder)
tools/usd2gltf/            usd2gltf.py (USD -> unflattened glTF, usd-core
                           in a git-ignored venv made by setup.sh)
tests/                     doctest unit tests
docs/decisions.md          technique decisions with sources
```

## Frame (renderer_frame.cpp, in order)

Instance table upload + TLAS build over all instances (only on the first
frame and after instances change; one TLAS shared by both frames; its
records are the placed instances', written once at setPlacement by
tlas_fill.comp, then the hand-placed ones', copied in before each build) ->
DrawCounters reset -> terrain select (terrain_select.comp, CDLOD: one
indirect dispatch per quadtree level from the root, level 16, down to 0;
frustum test against the node's min/max-pyramid bounds; split while the
camera is within the next finer level's range, else emit to the terrain
draw list) -> cull cells (cell_cull.comp phase 0: placement cells visible
last frame, frustum (displayed box) and the layers' draw distance; emits
chunks of up to 64 consecutive instance ids; the hand-placed instances
are chunks too) -> early cull (cull.comp, one workgroup per chunk, phase
0: instances visible last frame, frustum test (placed: the prototype's
sphere and draw distance) only, seeding (instance, level root) for each
DAG level of each mesh whose LOD band can hold the cut) -> traverse
(cluster_traverse.comp, breadth-first over each level's cluster group
BVH, one indirect dispatch per BVH depth over ping-pong node queues: a
node is skipped when its LOD bounds rule out every cluster below or it
is outside the frustum; one occluded by last frame's pyramid is queued
for phase 1's traversal; leaves emit work items of their group's
clusters) -> cull clusters (cluster_cull.comp, dispatched indirectly:
Nanite LOD cut + frustum test + occlusion against
last frame's pyramid seen from last frame's view, queuing what it hides
for a retest; fills 4 cluster draw lists: opaque/masked x
single/double-sided) -> atmosphere LUTs
(transmittance + multi-scattering only when the Atmosphere changes;
sky-view, aerial perspective, sky irradiance every frame) -> early scene
pass into HDR RGBA16F + RG16F motion + D32 depth (sky fullscreen, then
the terrain (terrain.mesh, 16 workgroups of 8x8 quads per node, so hills
reach the depth pyramid; not drawn again late), then
vkCmdDrawMeshTasksIndirectEXT per list, mesh.mesh, one workgroup per
cluster) -> depth pyramid (depth_pyramid.comp, min-reduced R32F) -> late
cull (cell cull phase 1: every cell against frustum + pyramid, writing
cell visibility; then every instance of the emitted chunks against
frustum + pyramid; the newly visible
and phase 0's queued nodes go through the traversal (nodes tested against
the new pyramid) and the cluster cull, and phase 0's queued clusters are
retested against the new pyramid; writes the per-instance visibility
buffer) ->
late scene pass (load; late lists, then blended instances CPU-sorted back
to front, mesh.vert) -> TAA resolve
(compute) -> luminance histogram + exposure adaptation (compute) -> AgX
tonemap + +-1 LSB dither into the sRGB swapchain. 2 frames in flight; the
HDR/depth/LUT/history images are shared between them, and barriers
account for that.

## Conventions

- Units: meters; y = 0 is sea level. The atmosphere works in km. Light in
  lux (sun) and cd/m^2; shading is pre-exposed with last frame's exposure
  (GPU buffer) so HDR values stay in FP16 range.
- Space: right-handed, Y up, camera looks down -Z. Reverse-Z with an infinite
  far plane (depth 1 = near; compare GREATER_OR_EQUAL; clear to 0).
  Mat4 is column-major `m[column][row]`.
- Camera-relative rendering: view and view-projection matrices on the GPU
  carry no translation (`Camera::viewRotation()`); shaders compute
  `world - cameraPosition` first (last frame: `previousCameraPosition`),
  and pass that between stages, culling included. World positions are
  rebuilt only where they are needed (textures, shadow rays, atmosphere).
- Motion vectors: UV units, `previous - current` (the DLSS convention).
- Curvature is a display warp: the world is flat (gameplay, `heightAt`,
  lighting, shadow rays, TLAS), but every rasterized vertex drops by
  `curvatureDrop` (common.glsl, d^2 / 2R, d horizontal distance from the
  camera it is seen from, R the Atmosphere's bottomRadius), current and
  previous position each with its own camera. Varyings carry the flat
  relative position; aerial perspective and the surface's atmosphere
  lookups (sun transmittance, sky luminance) use the displayed one.
  Culling tests displayed bounds (spheres: `displayedSphere` in
  cull.glsl; terrain node and cell boxes: `displayedBoxInFrustum` in
  common.glsl); LOD distances stay flat.
- GPU-driven scene: all meshes live in shared vertex/index pools (bump
  allocated); `MeshInfo` and `InstanceData` tables are on the GPU. One
  24-bit instance id space (`kMaxInstanceId`) serves culling, draw lists,
  visibility and the TLAS custom index: ids below `kPlacedBase` index
  `InstanceData`, the rest index the placed table (`PlacedInstance`, 24
  B, order (cell, layer, pattern point); its index is not a persistent
  id, (layer id, cell, point) is), each a prototype (`PrototypeInfo`:
  its meshes, bounds, BLAS); shaders read instances only through
  `loadInstance` (common.glsl), and a ray hit's mesh through
  `instanceMesh` (the hit's geometry index picks a prototype's mesh).
  The visibility buffer holds one uint per instance id, then one per
  cell. `SceneData` carries the placed table, the `Cell` table
  (64 m cells, row-major from the north-west of `cellGrid`) and the grid. Opaque/masked
  meshes are drawn as clusters of their LOD DAG (cluster, cluster-vertex
  and cluster-triangle pools; draw list entries are (instance, cluster));
  blended meshes and the BLAS use the original indices. Push constants
  carry the scene buffer address (plus the draw list index for
  mesh.mesh); everything else is reached through it (buffer device
  addresses).
  Descriptor sets: scene layout set 0 = atmosphere LUTs, set 1 = DFG LUT
  + bindless `textures[]` (also read by terrain.mesh: the terrain's
  R16_UNORM height image, via texelFetch so it matches `heightAt`
  exactly; the shadow march also reads it with textureGather through
  `nearestSampler`, whose CLAMP_TO_EDGE its mirror-tile math relies on,
  so don't switch that sampler to mirrored; and its RG16_SNORM normal map with a mirrored-repeat sampler); post layout set 0 = HDR/motion/history (two
  ping-pong sets); pyramid layout set 0 = source + destination level (one
  set per level); cull layout set 0 = depth pyramid (min sampler).
- Terrain: the heightfield's quadtree is centered on the origin (shifted
  under one sample so level 2's 2 m vertices land on samples); level 0
  nodes are 16 m of 32x32 quads, level 0's LOD range is 80 m, ranges
  double per level, vertices morph over the last 30% of their level's
  span. Levels 0-1 (finer than the data) add up to 0.3 m of detail noise.
  Nodes are drawn near to far (the selected list read backwards) so
  early-Z rejects hidden terrain. The terrain is not in the TLAS: sun
  shadows march its height pyramid (`terrainOccludes` in terrain.glsl,
  every surface) besides the TLAS query; terrain fragments start the ray
  on the bilinear heightfield, the march also above their cell's corners
  over the shading normal's plane (`terrainCellAbovePlane`, so a cell
  never shadows itself where the smooth normal faces the sun). Pyramid
  level 0 bounds each bilinear cell. The quadtree root is level 16 (~524 km
  half-size), covering the horizon up to ~20 km altitude.
- Any struct in `gpu_types.h` marked "must match" mirrors a declaration in
  `shaders/common.glsl`, `post.glsl` or `exposure.glsl` (or the shader it
  names: `GpuPlacementLayer`/`PlacementConstants` in placement.comp,
  `ScanConstants` in scan.comp, `TlasFillConstants` in tlas_fill.comp; `PlacedInstance` in placement.h); change
  both together (static_asserts check the sizes). `placement.glsl` mirrors
  `PlacementField`/`PlacementRule` (placement.h) and placement.cpp's
  integer core operation for operation: placement keep/reject must be
  bit-exact on CPU and GPU (no floats on that path).
- New shader: `add_shader(source output [DEFINES...])` in
  `engine/CMakeLists.txt`. Variants come from defines (e.g. `mesh.frag` ->
  `mesh_masked.frag` with `ALPHA_MASK`).
- Style: clang-format default-ish, 2-space indent. Comments only for a
  non-obvious *why*.
- Dependencies come from FetchContent with pinned tags, commits or hashes.
  Third-party targets get `-w`; the Basis Universal encoder is always
  `-O2 -fno-sanitize=all` (it's ~20x slower under ASan).

## Gotchas

- LeakSanitizer: libdecor's dbus/pango leaks are suppressed in
  `window.cpp`; NVIDIA driver leaks around device creation and acceleration
  structure builds are ignored with `DriverLeakScope` (gpu_types.h). Don't
  use DWARF unwinding to chase them; it makes startup 6x slower.
- Linking against the engine by hand also needs `-lsimdjson` (fastgltf uses
  the system simdjson) and `-lzstd`.
- git-lfs is not installed, so dependencies whose repos need LFS must come
  from release tarballs.
- Hyprland is configured in Lua: `hyprctl dispatch "hl.dsp.window.close({
  window = \"address:0x...\" })"`.
- Terrain data is swissALTI3D 2 m (swisstopo open government data):
  Napf region, E 2631..2645, N 1198..1212 (LV95 km), cooked to
  `assets/terrain/napf.height` (7500x7500, 16-bit). Any use must credit
  "© swisstopo". The heightfield maps crop center = origin, east = +x,
  north = -z; outside the grid indices mirror (MIRRORED_REPEAT).
- `assets/` is git-ignored. Re-create it with `tools/fetch-sample-assets.sh`
  (needs `jq`); once the project is built it also cooks every model and the
  terrain layers (`assets/terrain/<id>/*.ktx2`). Loading the Poly Haven
  models prints UBSan shift warnings from inside MikkTSpace (third-party
  code, not ours).
- Megascans trees, the Fab cliff and their import tools:
  `docs/assets.md`. The rock placement layer and the terrain material's
  rock blend share `kRockNormalY` (renderer.h).
- `loadGltf` caches each model's cluster LOD hierarchy as `<name>.clusters`
  next to the glTF, keyed by a hash of the loaded geometry and versioned
  with meshoptimizer; the first load after a change prints "Building
  clusters" and rewrites it (an unwritable cache only warns). The cache
  also stores each mesh's per-DAG-level group BVH (`nodes`, `levels`;
  version 3, so older caches rebuild once), uploaded to a shared node
  pool (`ClusterNodeInfo`, children rebased into the node or cluster
  pool) and level table. `selectClustersBvh` is the CPU reference for the
  GPU traversal (cull.comp's level band, cluster_traverse.comp's node
  test): it must select exactly the clusters `lodVisible` accepts.
- Known issues and deferred work live in `docs/backlog.md`, not here: add
  a row for anything deferred, remove it in the commit that resolves it.
