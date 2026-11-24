# Meridian: architecture reference

Where the code lives, what each frame does in order, and the data
layouts the shaders share. Read the section a change touches; keep it
current in the same commit as the change (CLAUDE.md working agreement).

## Layout

```
engine/include/meridian/   public API: renderer.h (Renderer, Lighting,
                           Atmosphere, materials), camera.h, mesh.h,
                           model.h (glTF), texture.h (KTX2), cluster.h
                           (cluster LOD DAG, LOD cut rule, group BVH),
                           terrain.h (Heightfield, height pyramid),
                           placement.h (rules, dither pattern,
                           evaluatePoint: CPU reference of GPU placement,
                           PlacedInstance), grade.h (Grade, CPU
                           reference of the grade + AgX LUT bake that
                           grade_lut.comp mirrors; Renderer::setGrade;
                           gradeToJson/gradeFromJson, the grade file),
                           upscaler.h (AaSettings, render/display rules:
                           scaledExtent, jitterPhaseCount,
                           textureMipBias), upscaler_vk.h (Upscaler,
                           UpscalerFrame, UpscalerProvider: the Vulkan
                           extension point for upscaler backends; the
                           Renderer constructor takes an optional
                           provider), weather.h (WeatherState,
                           presets, Weather: blend, wind integral,
                           history, stateAt; windVelocityAt;
                           cloudShadowBasis, Beer shadow texel), gi.h
                           (GI clipmap constants, slice mapping, SH L1:
                           the CPU reference of gi.glsl), window.h,
                           shadow_cascades.h (sun shadow cascades: light
                           basis, snapped windows, ortho projection and
                           planes, re-centre test), math/{vec3,mat4}.h
engine/dlss/               meridian_dlss (MERIDIAN_DLSS, default ON):
                           dlss.h (createProvider, attribution),
                           dlss.cpp (NGX SR/DLAA as an UpscalerProvider),
                           the SDK by FetchContent; the core never
                           includes or links NGX. The build copies the
                           NGX snippets, dlss_third_party_notices.txt and
                           game/credits.txt next to the game binary
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
  renderer_terrain_shadow.cpp  the terrain shadow-height map: window,
                           rebuild rule, terrain_shadow.comp dispatch
  renderer_placement.cpp   setPlacement: count, scan, write at load (two
                           submits), Debug check against evaluatePoint;
                           buildPlacedTlas: prototype BLAS + PrototypeInfo,
                           cell margin, TLAS fill (tlas_fill.comp) + build
  renderer_farfield.cpp    far field at setPlacement: prototype bake
                           (farfield_bake.comp, ray queries on the BLAS;
                           also each tree prototype's fine grid for its
                           voxel mips, protovoxel.h/.cpp),
                           splat of the placed table into world bricks
                           (farfield_splat.comp), column map, Debug checks
                           against the CPU replica (farfield.h/.cpp)
  renderer_shadows.cpp     sun shadow cascades: which re-render, their
                           SceneData copies (shadowView), the shadow-mode
                           cull chain and the depth-only mesh pass
  renderer_frame.cpp       per-frame: instance upload + TLAS build when
                           instances change, GPU culling, all passes in
                           draw()
  taa_upscaler.*           the core Upscaler backend: taa.comp and
                           sharpen.comp, their pipelines and the
                           ping-pong history
  renderer_clouds.cpp      cloud noise volumes (at load), weather map
                           window and pass, cloud shadow map placement
                           and pass, sky capture pass, dumpWeatherMap,
                           printAmbient, cloud march targets and
                           histories (quarter/half res) and the cloud
                           pass (march, resolve, composite)
  renderer_gi.cpp          GI probe and ground cache buffers, clipmap
                           windows per frame, the ground relight, probe
                           trace and albedo feedback dispatches
  renderer_reference.cpp   reference path tracer: accumulation buffers,
                           its pipeline, the per-frame dispatch, noise
engine/src/                model.cpp (glTF), texture.cpp (KTX2),
                           cluster.cpp + clusterlod.cpp (meshoptimizer),
                           terrain.cpp, placement.cpp, grade.cpp,
                           weather.cpp, window.cpp
engine/shaders/            GLSL; *.glsl are includes (see below). Terrain:
                           terrain.glsl (heightfield sampling + mirror,
                           quadtree, morph), terrain_detail.glsl (near
                           detail noise, also lifts streamed instances),
                           terrain_select.comp, terrain.mesh, terrain.frag
                           (mesh.frag with TERRAIN), terrain_normals.comp,
                           terrain_shadow.comp (the shadow-height map).
                           placement.glsl: integer keep/reject twin of
                           placement.cpp; placement.comp
                           (PLACEMENT_COUNT / PLACEMENT_WRITE variants)
                           and scan.comp (exclusive scan) run at load;
                           PLACEMENT_STREAM regenerates streamed slots
                           per frame;
                           tlas_fill.comp writes the placed TLAS records.
                           contact_shadows.comp: screen-space sun
                           shadows after the late scene pass.
                           cell_cull.comp: placement cells -> instance
                           chunks for cull.comp -> BVH seeds for
                           cluster_traverse.comp -> work items for
                           cluster_cull.comp.
                           farfield.glsl: brick lookup, the far-cell
                           march, farFieldTransmittance; voxel.mesh/.frag
                           draw far cells; protovoxel.glsl: prototype
                           voxel layout, switch rule, axis depths;
                           voxel_bricks.comp lists a voxel tree's bricks
                           and voxels, voxel_instance.mesh/.frag draw
                           them (voxel_band.frag: the switch band's
                           dither); lighting.glsl: the shading
                           mesh.frag, the voxel shaders and reference.comp
                           share; surface.glsl: material and terrain
                           surface evaluation (mesh.frag's and ray hits',
                           through SurfaceInput); ray_hit.glsl
                           (traceClosest: TLAS + heightfield hit) and
                           reference.comp, the reference path tracer.
                           wind.glsl (windVelocity, windOffset, gust),
                           weather.glsl (weatherAt: history lookup),
                           clouds.glsl (tileable noise, weatherMapAt +
                           analytic fallback, layer profiles,
                           cloudDensity, cloudShadow, skyCaptureAt),
                           clouds_march.glsl (layer span + lit march,
                           shared by clouds_march.comp and
                           sky_capture.comp), cloud_noise.comp,
                           weather_map.comp, cloud_shadow.comp,
                           cloud_shadow_blur.comp, clouds_march.comp,
                           clouds_resolve.comp, clouds_composite.comp,
                           clouds_temporal.glsl (their push constants,
                           cloud reprojection).
                           gi.glsl: probe buffer layout, clipmap
                           indexing, giSample/giIrradiance (the ambient
                           hook in lighting.glsl), the ground cache
                           (giGroundRadiance, giGroundSunVisibility);
                           gi_probes.comp traces the probes, gi_ground.comp
                           relights the ground cache, gi_albedo.comp
                           (scatter, resolve) feeds it the rendered albedo.
game/src/main.cpp          scene setup, fly camera, CLI options
game/src/overlay.{h,cpp}   grading panel (ImGui widgets), grade file
                           with sun, haze and bloom (Look), G toggle
game/src/perf_overlay.{h,cpp}  performance overlay (F3), CSV recording
                           (F4, --benchmark); frame_stats.h: lows
tools/cook/                meridian-cook; tree.cpp (--tree: simplify,
                           prune, flatten, leaf diffuse transmission;
                           textured materials keep UVs and maps),
                           ktx2.cpp (shared encoder, coverage mips)
tools/usd2gltf/            usd2gltf.py (USD -> unflattened glTF, usd-core
                           in a git-ignored venv made by setup.sh;
                           UsdUVTexture inputs -> glTF textures;
                           texture_tree.py: Megascans leaf atlas +
                           bark onto an untextured Megaplants glTF,
                           the game prefers `<tree>_tex` cooks)
tools/unreal/              export_trees.py (Unreal editor Python, headless:
                           inventory + glTF/PNG export, docs/assets.md),
                           cook_ready.py (export -> textured glTFs for
                           meridian-cook --tree)
tests/                     doctest unit tests
docs/                      see "Docs" at the top
```

Plant micro occlusion: leaf materials (`diffuseTransmission > 0`, the
Megascans plants, which ship no AO) get an occlusion that darkens toward
the tuft base (height 0..1 from the vertex's object-space y over the mesh
bound's top, varying location 8; mesh.vert sends 1, and so does mesh.mesh for
crowns, leaf meshes over 4 m tall), scaled by the micro
shadow strength: ambient and back irradiance via `s.occlusion`, sun
diffuse at half weight. Tree crowns (meshes over 4 m tall) also get a crown
sky visibility per vertex (mesh.mesh `crownVisibility`, location 12: the
transmittance of a uniform leaf medium filling the ellipsoid inside the
leaf mesh's bounding box, `MeshInfo` half extents, along three rays up and
outward), ambient only, same strength scale, and only with GI off (the
probes trace the canopy themselves; both would occlude it twice); a placed
tree's bark uses its leaf mesh's crown.
A leaf's sunlit side is its geometric normal's (mesh.frag, reference.comp:
pbrt's rule): where the shading normal disagrees, the Lambert lobe on that
side (front diffuse or transmission) takes |N.L|, GGX only where both face
the sun.

## Frame (renderer_frame.cpp, in order)

Instance table upload + TLAS build over all instances (only on the first
frame and after instances change; one TLAS shared by both frames; its
records are the placed instances', written once at setPlacement by
tlas_fill.comp, then the hand-placed ones', copied in before each build) -> terrain
shadow (terrain_shadow.comp, renderer_terrain_shadow.cpp, only when the sun
is above 1 degree and has turned more than 0.02 degrees, the camera moved
more than 1 km from the window centre, or on the first frame: the
shadow-height map, 2048^2 RG32F texels of 4 m in a sun-aligned window
around the camera, texel-snapped in the sun's frame; one 256-thread
workgroup per line toward the sun, bilinear heights every 2 m through the
window and every 4 m upstream as far as the terrain's relief over tan(sun
elevation) (at most 20 km), a parallel prefix max of H + a tanE storing
per texel the height h above which the sun gets through and the distance
to the occluder) ->
sun shadows (raster, the default; renderer_shadows.cpp: 5 cascades, 20 * 4^i m
around the camera, 2048^2 D32 each, texel-snapped on a fixed light-space
grid; a cascade re-renders only when the camera's offset from its centre
passes half its margin, or on the first frame, a sun change or an
instance change; each re-render runs the cell cull -> cull -> traverse ->
cluster cull chain on a copy of SceneData with `shadowView` set (light
box planes, no curvature, orthographic LOD of 1 texel, no occlusion, no
visibility writes, no streamed layers, no far switch, no voxel trees,
LOD error 0.25 texel in cascades 0-1 for thin foliage, the depth range up
to the terrain's highest sample + 150 m) into its own
counters and draw lists, then shadow.mesh (+ shadow_masked.frag for
alpha-masked) draws all four buckets, cull none, slope-scaled bias; the
coarsest cascade (`shadowFarCells`) skips every cell's trees and casts its
far cell instead: voxel.mesh's box, voxel_shadow.frag marching the bricks
along the sunlight to opacity 0.5) ->
DrawCounters reset -> terrain select (terrain_select.comp, CDLOD: one
indirect dispatch per quadtree level from the root, level 16, down to 0;
frustum test against the node's min/max-pyramid bounds; split while the
camera is within the next finer level's range, else emit to the terrain
draw list) -> stream (only with a streamed layer: placement_stream.comp
regenerates the ring slots whose cell left the window around the camera,
one workgroup per (slot, streamed layer)) -> cull cells (cell_cull.comp
phase 0: placement cells and streamed slot records visible
last frame, frustum (displayed box) and the layers' draw distance; emits
chunks of up to 64 consecutive instance ids; the hand-placed instances
are chunks too; with the far field, a cell whose 4 m voxel is under
~1 px skips its tree-layer chunks and joins the far list with its level)
-> early cull (cull.comp, one workgroup per chunk, phase
0: instances visible last frame, frustum test (placed: the prototype's
sphere and draw distance) only, seeding (instance, level root) for each
DAG level of each mesh whose LOD band can hold the cut; a tree past the
voxel switch (its prototype's finest voxel <= 2 px, both over a 10% band)
is listed as (instance, mip: the coarsest whose voxel is <= 2 px) instead,
and voxel_bricks.comp, one workgroup per tree, lists its bricks in the
frustum (with the terrain's sun visibility marched once per brick) and,
compacted, the voxels that draw: coverage by a stable hash of tree and
voxel against the voxel's opacity along the view, skipping voxels whose
baked optical depth toward the camera exceeds 3.5) -> traverse
(cluster_traverse.comp, breadth-first over each level's cluster group
BVH, one indirect dispatch per BVH depth over ping-pong node queues: a
node is skipped when its LOD bounds rule out every cluster below or it
is outside the frustum; one occluded by last frame's pyramid is queued
for phase 1's traversal; leaves emit work items of their group's
clusters) -> cull clusters (cluster_cull.comp, dispatched indirectly:
Nanite LOD cut + frustum test + occlusion against
last frame's pyramid seen from last frame's view, queuing what it hides
for a retest; fills 4 cluster draw lists: opaque/masked x
single/double-sided; a masked cluster of radius <= 1 m also marches the
terrain toward the sun once and packs the result into its entry's top 8
bits, which mesh.frag uses instead of its per-fragment march) -> atmosphere LUTs
(transmittance + multi-scattering only when the Atmosphere changes;
sky-view, aerial perspective every frame) -> weather (weather_map.comp:
512^2 over 60 km around the camera, texel-snapped; then the cloud shadow
map, cloud_shadow.comp: a Beer shadow map, 1024^2 over 30 km across the
sun, orthographic along it, texel-snapped in its frame on a grid that
moves with the cloud wind (so shadow edges glide instead of stepping a
texel), 32 steps per texel across the layer, only for lines that reach a
receiver within ~15 km of the camera, every other row per frame
(alternating) once a full map exists and the grid kept its texel (all of
it when the camera or the wind moves it a texel), and cloud_shadow_blur.comp, a bilateral 5x5 of its
extinction and optical depth; then sky_capture.comp: a coarse cloud march
from the camera into a 64^2 octahedral map (cloud luminance + haze,
transmittance; one texel of every 2x2 block per frame, all when the
history resets); then sky_irradiance.comp: the sky-view LUT with the capture's
clouds over it integrated into the scene's ambient irradiance, and the
LUT alone into the clear-sky irradiance that lights the clouds) -> gi probes
(unless `--gi off`: gi_probes.comp, one 64-ray workgroup per probe
that is stale, never traced or in this frame's share (hashed over columns and
slices: 1/8 of the finest level, 1/16 of the next, 1/32 of the coarser); L1 SH
with hysteresis 0.95, also written packed as half floats for the shading;
ground hits read last frame's cache (uncached: a neutral grey albedo);
foliage along a ray is lit where the ray meets it
(`farFieldTransmittanceScatter`: the far-field march also returns the
absorbed-weighted mean palette reflected and transmitted rgb and hit
distance; from that point a sun march (at most 64 m) and an upward sky
march, the spherical-leaf phase toward the probe (`leafPhase`, gi.h is its
CPU reference) and recollision 1 / (1 - p albedo) with p = 1 - sky
visibility); read by the scene passes) -> gi ground
(gi_ground.comp relights a quarter of the ground cache's texels per frame,
plus new or stale ones, with this frame's probes) -> early scene
pass into HDR RGBA16F + RG16F motion + R8 transparency mask + the Ray
Reconstruction guides (RGBA8 diffuse albedo, RGBA8 specular albedo, RGBA16F
world normal + roughness; null views unless the upscaler wants them, the diffuse albedo also when GI is on) + RGBA16F sun light (the
sun's exposed radiance, for contact shadows) + D32 depth
(sky fullscreen, then the masked lists depth-only (mesh_masked_depth.frag:
the alpha test alone, its texture fetch biased by `Effects::cutoutMipBias`, default +1 mip), then the terrain (terrain.mesh, 16 workgroups of 8x8 quads per node, so hills
reach the depth pyramid; not drawn again late), then
vkCmdDrawMeshTasksIndirectEXT per list, mesh.mesh, one workgroup per
cluster: the opaque lists, then the masked lists shaded with depth EQUAL,
no discard and early fragment tests, so hidden grass and the terrain
under it aren't shaded; the late pass orders its lists the same way; then the voxel trees: voxel_instance.mesh, 64 voxels per
workgroup, each a camera-facing quad of the cube's projected area,
voxel_instance.frag shades it flat (no discard, so early depth holds;
trees in the switch band take voxel_band.frag's dither discard, which
the masked clusters' depth pass complements; opaque clusters, such as the
traced tree leaves, draw whole there, under the dithered voxels); then
the far field pass: voxel.mesh emits each far cell's
canopy box, voxel.frag marches its bricks (farfield.glsl) to opacity 0.5
and writes the hit's depth (conservative, depth_less), motion and the
same attachments) -> depth pyramid (depth_pyramid.comp, min-reduced R32F) -> late
cull (cell cull phase 1: every cell against frustum + pyramid, writing
cell visibility; then every instance of the emitted chunks against
frustum + pyramid; the newly visible
and phase 0's queued nodes go through the traversal (nodes tested against
the new pyramid) and the cluster cull, and phase 0's queued clusters are
retested against the new pyramid; writes the per-instance visibility
buffer) ->
late scene pass (load; late lists (masked depth, opaque, masked shaded), the late voxel trees, then blended instances CPU-sorted back
to front, mesh.vert) -> clouds (clouds_march.comp at quarter
resolution, half inside the layer or within 250 m of it, each texel's ray
offset within the texel's central half per frame (R2 sequence): luminance with
haze + transmittance, mean cloud depth d, stored -1 - d where scene
geometry cut the march; the cirrus sheet (`cirrusLayer`) behind it on
sky pixels; clouds_resolve.comp accumulates it into a per-resolution history
(current weight 0.03 for a still view up to 0.1 at 0.5 texel per frame of
motion, 0.04-0.25 in the layer; rescaled on exposure change)
reprojected by cloud depth + wind; clouds_composite.comp upsamples it
bilaterally on scene depth, sharpens it where every tap agrees (the
outline: an unsharp mask of opacity and shading against a radius-2-texel
tent, `Effects::cloudOutline`, push constant, off by default) and composites it over HDR (as storage, GENERAL), writes
cloud motion where opacity > 0.5 and raises the transparency mask to the
cloud coverage) -> contact shadows (contact_shadows.comp: with the
complete depth, a 16-step march toward the sun over 0.25-1.5 m; where it
hits the depth buffer, the pixel's exposed sun radiance (scene attachment
6, written by mesh.frag: the sun branch after haze, pre-exposed; 0 for sky
and blended surfaces) is subtracted from the HDR colour, so ambient stays;
faded out between 40 and 60 m; after the clouds, which stand in front of
a surface that near only inside the layer) -> upscaler (`Upscaler::evaluate` with an
`UpscalerFrame`: colour, depth, motion, transparency mask (and the guides
when `wantsGuides()`) in SHADER_READ_ONLY, jitter, unjittered matrices, reset; writes the
display-sized RGBA16F `output`; the backend is DLSS (meridian_dlss,
`NGX_VULKAN_EVALUATE_DLSS_EXT`, or `NGX_VULKAN_EVALUATE_DLSSD_EXT` under
Ray Reconstruction) or `TaaUpscaler`: taa.comp, which writes
its resolve to its own history (pre-exposed, so rescaled by
`ExposureState` exposure / previousExposure; with `Effects::stableTaa`, the
default, the current frame is reconstructed from the 3x3's jittered samples
and the history resampled by Catmull-Rom), then sharpen.comp, which
writes `output` from it (a copy under stable TAA; otherwise CAS scaled by each pixel's motion: a copy where still, a quarter of CAS's weakest lobe from 0.5 px/frame),
both inside the `aa` pass)
-> bloom (bloom.comp, glare: 13-tap downsamples of `output` into a
half-display RGBA16F mip chain down to ~1/128, tent upsamples mixing each
coarser level in by `scatter`, then `output` = mix(output, level 0,
`intensity`), in place; energy-conserving, before the exposure meter so
it meters the glare as the eye does; `Renderer::setBloom`, intensity 0
skips it) -> luminance histogram + exposure adaptation (compute; the
target is mean-to-grey, bounded above by the 95th percentile and below by
3 EV over an open landscape's meter at the camera's light, the
open-ground illuminance sky_irradiance.comp writes) -> grade
LUT bake (grade_lut.comp: grade + AgX into a 65^3 RGBA16F 3D LUT; frame 1
and after `setGrade`, timed with the tonemap; Debug prints `grade LUT
check: max diff` against `bakeGradeLut`, which must stay <= 2e-3) ->
tonemap (one trilinear LUT fetch, indexed by the log2 shaper of the
AgX-inset colour) + +-1 LSB dither into the sRGB swapchain -> overlay
(only after `Renderer::beginOverlay()`: ImGui drawn onto the swapchain in a
second rendering pass, style colours linearized because the target
encodes sRGB; no pass otherwise, so a hidden overlay costs nothing;
captures include it). Everything up
to the upscaler runs at the render size (`renderExtent`: depth, HDR,
motion, depth pyramid, cloud targets at 1/4 or 1/2 of it, Halton(2,3) jitter in render pixels over
`jitterPhaseCount` phases, material mip bias `textureMipBias`, both in
upscaler.h); the upscaler's output and everything after it at the
display size (swapchain). Both sets of images, and the upscaler, are
created in `createSwapchain` and destroyed in `destroySwapchain`;
`setAntiAliasing` recreates them. 2 frames in flight; the
HDR/depth/LUT/history/output images are shared between them, and
barriers account for that.

## Data conventions

Rules every change follows are in CLAUDE.md "Conventions"; these are
the layouts behind them.

- Transparency mask: scene attachment 2 (R8, render-sized, cleared to 0
  in the early pass); only the blended pipeline writes it (coverage,
  blend op MAX), all others have write mask 0; the cloud composite raises
  it to the cloud coverage (`max`, as a storage image).
- Scene attachments: 0 HDR RGBA16F, 1 motion RG16F, 2 transparency mask
  R8, 3 diffuse albedo RGBA8 (`diffuseColor`), 4 specular albedo RGBA8
  (the DFG pre-integrated `mix(dfg.x, dfg.y, f0)`), 5 normal RGBA16F (world
  `N`, facing the viewer on double-sided surfaces) with perceptual roughness
  in alpha; 6-7 are reserved. Sky writes 0.5 / 0 / 0; the blended pipeline
  masks 3-5 off. The guide images (plus a render-sized R16F specular hit
  distance cleared to 65504, the guide's sky value: no ray-traced
  reflections) exist only when the upscaler `wantsGuides()` (Ray
  Reconstruction); otherwise those views are null and the writes are
  discarded (device extension `VK_EXT_dynamic_rendering_unused_attachments`).
- `SceneData.whiteNoise` (1 under Ray Reconstruction, else 0): at 1 the
  sun-disk sample (and contact shadows) use a PCG3D hash of (pixel,
  frameIndex) (`pcg3d`, common.glsl) instead of interleaved gradient noise.
  The cloud march keeps interleaved gradient noise: its own resolve
  integrates it, and RR sees only the resolved clouds.
- GI probes (`SceneData.giMode`, `gi`: the probe buffer, a 176 B header of
  per-level windows (probes and ground cache) + 64 B probes, toroidal by
  world column, each stamped with the column it was traced for; the
  shading reads a packed copy instead (24 B per probe, half floats, slices
  counted from the terrain under the shaded point); the ground
  cache is a second buffer, 48 B per texel + a 16 B albedo accumulator,
  toroidal by world texel and stamped the same way). The diffuse albedo
  guide (scene attachment 3) exists whenever GI is on, not only under Ray
  Reconstruction (`Renderer::setGiMode` recreates the targets if it is
  turned on after they were made); the other guides still only for the
  upscaler. gi albedo runs after the clouds, before contact shadows, with
  depth and the albedo in SHADER_READ_ONLY: `addAmbientLight` takes the diffuse
  and rough-specular irradiance from `giSample`/`giIrradiance` when giMode
  is 1, blending to the analytic sky + ground term outside the clipmap, and
  occludes the sharp sky reflection by the probes' rough irradiance over
  the open sky's (under a canopy the sky is not reflected at full strength).
- GPU-driven scene: all meshes live in shared vertex/index pools (bump
  allocated; they and the cluster pools grow by >= 1.5x when an upload
  needs more, `grownPool`); `MeshInfo` and `InstanceData` tables are on
  the GPU. One 24-bit instance id space (`kMaxInstanceId`) serves
  culling, draw lists, visibility and the TLAS custom index: ids below
  `kPlacedBase` index `InstanceData`, the rest index the placed table
  (`PlacedInstance`, 24 B, order (cell, layer, pattern point); its
  index is not a persistent id, (layer id, cell, point) is), each a
  prototype (`PrototypeInfo`:
  its meshes, bounds, BLAS); shaders read instances only through
  `loadInstance` (common.glsl), and a ray hit's mesh through
  `instanceMesh` (the hit's geometry index picks a prototype's mesh).
  Streamed layers (`PlacementLayer::streamFullDensity > 0`) live in a
  ring after the static table: per (window slot, streamed layer) a fixed
  run of pattern-size slots plus a `Cell` record after the grid's; the
  window is W x W 64 m cells around the camera (`streamWindowCells`),
  regenerated by the same integer rules when a cell enters it. For
  streamed ids `loadInstance` thins by distance (dither threshold vs
  `streamKeepFraction`: fade-out over the last 50% of the draw distance),
  shrinks before culling (`streamFade`, band 0.3; `LoadedInstance.fade`
  reaches mesh.frag, which blends plants' normal and micro occlusion
  toward the terrain's) and lifts them onto the near detail noise; they
  are not in the TLAS (no
  ray-traced shadows cast, contact shadows only).
  The visibility buffer holds one uint per instance id (static, then
  ring), then one per grid cell, then one per streamed record. `SceneData` carries the placed table, the `Cell` table
  (64 m cells, row-major from the north-west of `cellGrid`) and the grid. Opaque/masked
  meshes are drawn as clusters of their LOD DAG (cluster, cluster-vertex
  and cluster-triangle pools; draw list entries are (instance | terrain sun code << 24, cluster):
  code 0 shadowed, 1 lit, 255 = mesh.frag marches the terrain per fragment);
  blended meshes and the BLAS use the original indices. Push constants
  carry the scene buffer address (plus the draw list index for
  mesh.mesh); everything else is reached through it (buffer device
  addresses).
  Descriptor sets: scene layout set 0 = atmosphere LUTs, set 1 = DFG LUT
  + bindless `textures[]` (also read by terrain.mesh: the terrain's
  R16_UNORM height image, via texelFetch so it matches `heightAt`
  exactly; the shadow march also reads it with textureGather through
  `nearestSampler`, whose CLAMP_TO_EDGE its mirror-tile math relies on,
  so don't switch that sampler to mirrored; and its RG16_SNORM normal map with a mirrored-repeat sampler); post layout set 0 = upscaler output
  (`resolved`), grade LUT sampled and as storage (one set); TaaUpscaler
  set 0 = HDR, motion, history read/write, output (two ping-pong sets,
  shared by taa.comp and sharpen.comp); contact shadows layout set 0 = depth, sun light, HDR (storage); pyramid layout set 0 = source + destination level (one
  set per level); cull layout set 0 = depth pyramid (min sampler), set 1 = bindless (the
  height image for cluster_cull.comp's terrain march); cloud
  layout set 0 = the cloud noise, weather map and cloud shadow map (raw,
  blurred) storage images, set 1 = bindless. The bindless table also
  holds the cloud noise volumes as 3D views, read through `textures3D[]`
  (clouds.glsl, same binding), the weather map and the blurred cloud
  shadow map; all stay in GENERAL layout. Cloud march layout:
  set 0 = atmosphere LUTs, set 1 = bindless, set 2 = scene depth + the
  march targets, the history to read (sampled) and to write, HDR and
  motion (storage), all GENERAL; one set per resolution and frame parity
  (`r * 2 + frameNumber % 2`), shared by the march, resolve and composite
  (push constants `CloudConstants`: scene, historyValid, inLayer, outline).
- Terrain: the heightfield's quadtree is centered on the origin (shifted
  under one sample so level 2's 2 m vertices land on samples); level 0
  nodes are 16 m of 32x32 quads, level 0's LOD range is 80 m, ranges
  double per level, vertices morph over the last 30% of their level's
  span. Levels 0-1 (finer than the data) add up to 0.3 m of detail noise.
  Nodes are drawn near to far (the selected list read backwards) so
  early-Z rejects hidden terrain. The terrain is not in the TLAS: sun
  shadows read the shadow-height map (`terrainShadowVisibility` in
  terrain.glsl: one bilinear fetch, soft over the sun disk's penumbra at
  the occluder's distance) where it serves (sun above 1 degree, receiver
  inside the window), else march its height pyramid (`terrainOccludes` in
  terrain.glsl, every surface; cluster_cull.comp and voxel_bricks.comp
  read the map too, against their per-cluster or per-brick disk sample's
  hash; gi_probes.comp always marches) besides the TLAS query; terrain fragments start the ray
  on the bilinear heightfield, the march also above their cell's corners
  over the shading normal's plane (`terrainCellAbovePlane`, so a cell
  never shadows itself where the smooth normal faces the sun). Pyramid
  level 0 bounds each bilinear cell. The quadtree root is level 16 (~524 km
  half-size), covering the horizon up to ~20 km altitude.
  Parallax occlusion mapping (`parallaxHit` in surface.glsl) marches the
  dominant ground layer's height (`TerrainLayer::heightScale`, meters,
  from the Megascans JSON) and shifts every layer's sample position
  (`terrainParallax`) by the same world offset; depth, motion vectors and
  the RR guides stay the raster surface's.
