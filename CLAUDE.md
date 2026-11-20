# Meridian

C++20 / Vulkan 1.3 game engine (`engine/`, static library `meridian`) plus
the game built on it (`game/`). Goal: photorealistic, first-person, open
world, grand scale. Hardware ray tracing is required. The setting is open,
so systems stay parametric (e.g. the atmosphere supports other planets).

Docs, read on demand:

- `docs/decisions.md`: technique choices and sources, newest last. Read
  the relevant entry before re-researching anything (`grep -n '^## '`
  lists them).
- `docs/backlog.md`: known issues and deferred work.
- `docs/superpowers/resume.md`: current state, open branches, next steps.
- `docs/superpowers/{specs,plans}/`: designs and implementation plans
  (Status line at the top); milestones in
  `specs/2026-11-19-showcase-roadmap.md`.
- `docs/assets.md`: Megascans and Fab asset import.

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
# the build uses ccache and mold automatically when installed (no setup needed)
flock /tmp/meridian-gpu.lock ./build/game/game   # interactive (WASD, mouse, Esc; F walk/fly)
./tools/fetch-sample-assets.sh                   # test models into assets/; also the Napf terrain (225 tiles, needs magick)
python3 -I tools/image-gates.py laplacian motion.png   # laplacian|ratio|shimmer|ssim|diff
tools/motion-gates.sh build-release out-dir      # motion/shimmer/SSIM gate table, GPU passes
./build/tools/cook/meridian-cook in.gltf assets/Name   # PNG/JPEG -> KTX2 glTF (bakes a lone node's rotation, scale + vertical offset)
./build/tools/cook/meridian-cook --texture in.jpg out.ktx2 color|data|normal
./build/tools/cook/meridian-cook --heightfield in.f32 W H SPACING out.height   # float32 m -> 16-bit .height
cmake -S . -B build-brute -DMERIDIAN_TERRAIN_MARCH_BRUTE_FORCE=ON   # A/B: terrain shadows by a 1 m linear march
cmake -S . -B build-off -DMERIDIAN_DLSS=OFF      # licence-clean build: no NVIDIA SDK, TAA only
python3 -I tools/region-luminance.py in.pfm X0 Y0 X1 Y1 [...]   # mean luminance per pixel rect (top-left origin, end exclusive) + ratio to the first
tools/cloud-compare.sh build-release out-dir     # cloud look views beside the reference photos + measured targets (tools/cloud-metrics.py)
```

Every run of the game binary (interactive, capture, perf; Debug or
Release) goes through `flock /tmp/meridian-gpu.lock`, because two Claude
instances may render in parallel and GPU timings would collide. Hold it
only for the run, never for builds. Examples below omit the prefix.

Controls: WASD, mouse, Esc. Fly mode: Space/Ctrl up/down, 30 m/s,
Shift x5. `F` toggles walk mode: eye 1.7 m above the ground, WASD horizontal at 1.4 m/s (Shift 5 m/s), no vertical keys.
`G` opens the grading panel (Dear ImGui overlay): every `Grade` field,
sun azimuth/elevation, haze, bloom (intensity, scatter) and an Effects
section (`Effects`, renderer.h, `Renderer::setEffects`: parallax on/off + depth x0..4,
parallax self-shadow, micro shadows on/off + strength (also darkens plant bases, below), contact shadows on/off
(off skips the dispatch) + length x0..3, bloom on/off), with Save, Load, Reset and a "show built-in
grade" A/B box. It frees the cursor and freezes the camera while open;
changes apply at once. Save writes the grade file,
`$XDG_CONFIG_HOME/meridian/grade.json` (default
`~/.config/meridian/grade.json`, shared by every checkout; flat JSON, field
names as in `grade.h` plus `sunAzimuthDegrees`, `sunElevationDegrees`,
`haze`, `bloomIntensity`, `bloomScatter`, `effect*` keys; missing = defaults), which **interactive runs load at
startup** (explicit `--sun`, `--haze` and `--bloom` still win); capture runs ignore it so gates stay deterministic.
`--grade FILE` loads a grade file in any run (A/B captures of a tuned
look); `--overlay grade|perf` opens a panel at startup (it is in captures;
repeat the flag for both).
`F3` toggles the performance overlay (passive, top right, the cursor stays
captured): FPS, frame / CPU / GPU ms, per-pass GPU ms, a 5 s frame-time
graph, AVG / 0.1% / 1% / 5% low / MAX FPS over the last 10 s. x% low =
1000 / mean of the slowest ceil(x% n) frames (`frame_stats.h`). `F4` (or its
Record button) records every frame to `meridian-<timestamp>.csv` in the
working directory (`frame,frame_ms,cpu_ms,gpu_ms,<one column per GPU
pass>`; the path is relative to the cwd; frame_ms is present to present, cpu_ms is frame_ms minus the time
blocked on fence and acquire, GPU columns are the newest finished frame,
2 frames behind) and prints the summary on stop; a recording still
running at exit is written too, and on a write error the run keeps it
in memory and reports. `--benchmark SECONDS FILE.csv` (SECONDS > 0; not
with `--capture` or the camera velocities) is a timed run with real
frame times, static camera and no camera input (Esc still quits; the
partial recording is written): FILE.csv is checked writable up front, 60
warm-up frames, then SECONDS recorded; writes the CSV, prints the
summary, exits.

Automated visual check (deterministic, ~4 s, exit code 1 on any Vulkan
validation error):

```bash
./build/game/game --camera X Y Z YAW PITCH [--sun X Y Z] [--scatter N] [--trees] [--lod-error PX] --capture out.png [--frames N]
```

A `.pfm` capture path writes the linear HDR image the tonemap reads
(the upscaler's output, pre-exposed) instead, for luminance ratios.

Motion sharpness: add `--camera-velocity X Y Z` (m/s) and/or
`--camera-angular-velocity YAW PITCH` (rad/s; `--camera-yaw-rate YAW` is
short for `YAW 0`), fixed 1/60 s steps ending at
`--camera`, and compare with the still capture of the same view:
`python3 -I tools/image-gates.py laplacian motion.png` (Laplacian std:
the gate is the motion frame's absolute sharpness, minimums anchored to
TAA v2; `ratio` is the informational motion/still column; also
`shimmer`, `ssim`, `diff`). The whole motion gate set runs with `tools/motion-gates.sh BUILD OUT [game args]`
(`GPU_RUNS=5` adds per-pass GPU medians). `--capture-sequence N` also
saves `<capture stem>_NNN.png` every N frames (fly-throughs,
time-lapses); per-pass GPU ms of such runs are inflated by the readbacks.

`--haze clean|summer|summer-hazy` picks the aerosol (default summer:
Swiss summer haze kept low in the valleys, blue zenith; summer-hazy:
haze through the ~2 km boundary layer, ~45 km visibility on the Napf ridges, grey sky;
clean = Bruneton's very clean defaults, for A/B).

`--aa dlss|taa` picks the upscaler: DLSS (default; falls back to TAA
with the log line `DLSS unavailable: <reason>; using TAA` when NGX
reports no Super Resolution). `--rr on|off` (default on: RR removes the
ray-traced shadow shimmer, decisions.md) picks Ray Reconstruction under DLSS
(preset F, `kRayReconstructionPreset` in dlss.cpp; falls back to Super
Resolution DLAA with `DLSS Ray Reconstruction unavailable; using Super
Resolution`). `--dlss-mode dlaa|quality|balanced|performance` (default
dlaa; RR DLAA is ~4.7 ms at 1440p) and `--dlss-preset E|F|J|K|L|M` (Super
Resolution only; default: M for DLAA, Quality and Balanced, NGX's own
for Performance, `defaultPreset` in dlss.cpp; A/B override) set DLSS. `--render-scale S` (0 < S <= 1, default 1) renders the scene
at the display size times S for TAA (`--aa taa`); DLSS picks its own render size.
Startup prints `Render WxH, display WxH, jitter phases N, mip bias B,
DLSS|DLSS Ray Reconstruction|TAA` and, with the DLSS module, `DLSS: SDK
310.9.16, Super Resolution ..., Ray Reconstruction ... (preset F)`.

Weather: `--weather clear|fair|broken|overcast` (default `fair`),
`--time-scale X` (game seconds per real second), `--weather-front
SECONDS` (blend from the start preset to overcast over that many game
seconds; the change enters at the front line 60 km upwind and travels
with the wind). Capture runs print the exposure (EV100) and the sky
irradiance with clouds and clear (x sun), and the weather at the camera (coverage,
type, wind speed and the direction it comes from, gustiness, game time).
`--clouds off` skips the cloud march, composite, cloud shadows and sky
capture (A/B). Ambient light (diffuse irradiance, the specular sky
fallback) includes the clouds through the sky capture, taken at the
camera. Front time-lapse: `--weather fair --weather-front 600
--time-scale 600 --capture-sequence 20 --frames 1200` from the ground or
above-layer view (the overcast deck reaches the camera after ~800 frames).
Surfaces (mesh.frag, terrain too) multiply the sun by `cloudShadow` at
their flat world position: the shadow map within ~13.8 km of the camera
(horizontally and across the sun), blended over 1.5 km into the
coverage-based fallback beyond (`1 - k R` per crossing, darkened by the
number of clouds a low sun's ray crosses; calibrated against the map);
the map fades out as the sun sets (6 to 3 degrees).
`--dump-weather-map out.png` saves the weather map after the last frame
(R coverage, G type, B precipitation; row 0 = north) and prints its
window; two dumps at `--time-scale 0` and `--time-scale 60 --frames 60`
show one game minute of drift.

`--lod-error PX` sets the LOD threshold (default 1 pixel; 0 = full
detail, for A/B); `--bloom INTENSITY SCATTER` sets the glare (default
0.015 0.7; `--bloom 0 0` = off, for A/B); `--effects pom=0,pom-depth=3,pom-shadow=0,micro=0,micro-strength=.5,contact=0,contact-length=2,bloom=0`
sets the live effect toggles (defaults = normal rendering; for A/B); `--no-occlusion` draws everything in the frustum
(occlusion culling A/B). Capture runs print drawn instances, clusters,
triangles and entries dropped by full cull lists (nonzero = missing
geometry), per list: chunks, work items, clusters, terrain nodes, bvh
nodes.
They also print drawn terrain nodes (2048 triangles each), `cells N`
(placement cells visible), `far cells N` (cells drawn as far-field
voxels), `bvh nodes N` (cluster group BVH nodes the
traversal visited), `placed N`
(placed instances), and `GPU passes:`
with GPU ms per pass (upload + tlas, terrain select, stream (only with a streamed layer), cull cells, cull early, traverse, cull clusters, atmosphere, weather, scene early, far field, pyramid, cull late, scene late, clouds, contact shadows, aa (the upscaler), bloom, exposure,
tonemap, overlay when a panel is open; they sum to the total). A
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
Laplacian with the still's). Placed forests (seed 1, all of these need `--trees`): forest edge
`600 1205.6 -400 -1.55 0.02`; forest interior `85 1158.6 -53 -1.55
0.25`; forest overview `700 1450 -400 -1.55 -0.5`;
canopy from 40 m above `85 1200 -53 -1.55 -1.2`; a
placed moss rock close up `1944.3 953.4 -1923.9 -1.55 -0.4`; trees on
the summit ridge at ~1.5 km: the open view toward the summit.
`--trees` brings the trees back; by default they are hidden (no tree
prototypes loaded, no forest or lone-tree layers, no forest floor in the
terrain material, no tree models in `--scatter`), until they are textured
and keep their coverage (`docs/backlog.md`).
Clouds (layer 1.7-4.8 km above sea level, cirrus sheet at 9 km): beside
the Chlorophyll frames: fair from 1500 m `1119 1500 -431 -1.55 0.05`, cirrus `1119 1356 -431 0
0.5`, stratocumulus the fair view with `--weather broken`, overcast
`1119 1356 -431 -1.55 0.25 --weather overcast`; fair cumulus from the
ground `1119 1355.9 -431 -1.55 0.15`; above the layer `1119 6500 -431
-1.55 -0.25` (pitch -0.1 looks across the weather map's 30 km edge);
inside the layer (half-resolution march) `1119 3200 -431 -1.55 0`; the
summit silhouette against clouds `1119 1356 -431 -1.55 0.03`; look
development (`tools/cloud-compare.sh`, each beside its photo in
`assets/reference/clouds/`, git-ignored): midday from the ground looking
up `1119 1356 -431 -1.55 0.5 --sun 0.45 0.85 0.25` (cloud1, cloud5), the
same toward the horizon at pitch 0.1 (cloud3, cloud4), sunset from above
the layer `1119 5500 -431 -1.55 -0.2 --sun -0.998 0.05 0.03 --weather
broken` (cloud2);
the script also captures `.pfm`s with clouds on and off and prints the
sunlit tops and the bases over the clear sky behind them, the cover, the
spread of the bases and the shading contrast, and the same measured on
the photos (display-referred, so compare the capture's image line); dusk
clouds `1119 1355.9 -431 -1.55 0.15 --sun -0.7 0.035 0.7` (sun 2 deg up;
`-0.05` for just below the horizon). Cloud motion checks (Laplacian vs
the still, and vs `--clouds off`): fast turn, the fair view with
`--camera-yaw-rate 1`; toward the layer `1119 1900 -431 -1.55 0.1
--camera-velocity -60 20 0`; across the base (quarter -> half switch at
2550 m, base 2800 m) `1119 2950 -431 -1.55 0.1 --camera-velocity -30 250
0` (with `--capture-sequence 2` for the sequence); summit silhouette
with clouds drifting across the view `1119 1356 -431 2.5 0.12
--time-scale 1.5` (~10 m/s, 80% lateral; still: `--time-scale 0`;
Laplacian on the crop `1000x340+900+560`, since whole frames are
dominated by terrain); strafing past the marble wall's top
`1119 1356 -431 -1.55 0.03 --time-scale 1.5 --camera-velocity 0 0 10`.
`--scatter N` places N random Poly Haven models and imported Megaplants
trees within 150 m of the start (fixed seed; loading 21 models, 14M
triangles, makes Debug startup ~2.5 min); over the scatter:
`1119 1362 -431 -1.55 -0.25`; a 27 m beech (with `--scatter 100`):
`1115.47 1366.85 -429.72 1.223 0`. Missing models drop out of the pick,
so without the Megaplants trees the same seed places different content
and these views differ.
The game places rocks over the whole Napf at load (`placeScenery` in
main.cpp; 21281 instances), poppies, goldenrod and ferns (~500k
together), streamed meadow layers around the camera (a short grass mat,
seed-head tufts in patches, hawkbit drifts (grasses and hawkbit graded
by `Prototype::tint`), clover: ring ~4.2M slots,
~1.1M live; building their dense dither patterns adds ~10 s to Release
and ~59 s to Debug startup on the first run only: the patterns are cached,
see Gotchas), plus forests and lone trees with `--trees`
(~2.65M instances), which loads the rocks (and with `--trees` the
Megaplants trees) as prototypes: Debug startup
~95 s, Release ~18 s. `--placement off` skips it (Release ~11 s:
heightfield pyramid, normal map and transcoding the terrain's 4K
textures, ~8.7 s of it); use it for captures that
don't need placement. Debug runs print `placement check:` (GPU table vs
the CPU reference, must be 0 mismatches), `stream check:` (every
regenerated streamed cell vs the CPU reference, must be 0 mismatches) and
`placement hash 0x...` of the static table (0x341d51799d224d3c for seed 1 without
trees, 0xe73211868a49a82f with `--trees`, every asset present). Capture runs print `streamed: L layers,
window WxW, ring C slots, N live`. Loading
placement also prints `TLAS:` (records, sizes, build ms, prototype BLAS
before and after compaction, cell margin). `--far-field off` draws
every tree as clusters instead of distant forest cells as voxels (A/B).
`--far-shadows tlas|voxel` (default voxel): sun shadow rays from receivers
beyond `kFarShadowDistance` (4 km, lighting.glsl) skip the far-field trees
in the TLAS and march the far field's voxels instead; `tlas` traces every
tree for every receiver (A/B; also what `--far-field off` does).

## Testing policy (usage matters)

- Always build. Run tests when logic they cover changes.
- Capture only when rendered output can change, with the fewest views that
  prove the change. The capture run's output already reports validation
  errors, leaks and GPU time; don't do separate runs for those.
- GPU ms are only comparable at the same resolution. The game runs
  fullscreen on the primary monitor by default, so captures share one size
  (the 1440p monitor; Hyprland's 1920x1080 FALLBACK output when it's off).
  `--windowed` gives a tiled window, whose size varies between runs.
  Fullscreen sizes the window to the monitor mode from the start
  (`Window::framebufferSize`), so startup prints one `Swapchain:` line.
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
                           cloudShadowBasis, Beer shadow texel), window.h,
                           math/{vec3,mat4}.h
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
  renderer_placement.cpp   setPlacement: count, scan, write at load (two
                           submits), Debug check against evaluatePoint;
                           buildPlacedTlas: prototype BLAS + PrototypeInfo,
                           cell margin, TLAS fill (tlas_fill.comp) + build
  renderer_farfield.cpp    far field at setPlacement: prototype bake
                           (farfield_bake.comp, ray queries on the BLAS),
                           splat of the placed table into world bricks
                           (farfield_splat.comp), column map, Debug checks
                           against the CPU replica (farfield.h/.cpp)
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
engine/src/                model.cpp (glTF), texture.cpp (KTX2),
                           cluster.cpp + clusterlod.cpp (meshoptimizer),
                           terrain.cpp, placement.cpp, grade.cpp,
                           weather.cpp, window.cpp
engine/shaders/            GLSL; *.glsl are includes (see below). Terrain:
                           terrain.glsl (heightfield sampling + mirror,
                           quadtree, morph), terrain_detail.glsl (near
                           detail noise, also lifts streamed instances),
                           terrain_select.comp, terrain.mesh, terrain.frag
                           (mesh.frag with TERRAIN), terrain_normals.comp.
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
                           draw far cells; lighting.glsl: the shading
                           mesh.frag and voxel.frag share.
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
                           cloud reprojection)
game/src/main.cpp          scene setup, fly camera, CLI options
game/src/overlay.{h,cpp}   grading panel (ImGui widgets), grade file
                           with sun, haze and bloom (Look), G toggle
game/src/perf_overlay.{h,cpp}  performance overlay (F3), CSV recording
                           (F4, --benchmark); frame_stats.h: lows
tools/cook/                meridian-cook; tree.cpp (--tree: simplify,
                           prune, flatten, leaf diffuse transmission),
                           ktx2.cpp (shared encoder)
tools/usd2gltf/            usd2gltf.py (USD -> unflattened glTF, usd-core
                           in a git-ignored venv made by setup.sh)
tests/                     doctest unit tests
docs/                      see "Docs" at the top
```

Plant micro occlusion: leaf materials (`diffuseTransmission > 0`, the
Megascans plants, which ship no AO) get an occlusion that darkens toward
the tuft base (height 0..1 from the vertex's object-space y over the mesh
bound's top, varying location 8; mesh.vert sends 1), scaled by the micro
shadow strength: ambient and back irradiance via `s.occlusion`, sun
diffuse at half weight.

## Frame (renderer_frame.cpp, in order)

Instance table upload + TLAS build over all instances (only on the first
frame and after instances change; one TLAS shared by both frames; its
records are the placed instances', written once at setPlacement by
tlas_fill.comp, then the hand-placed ones', copied in before each build) ->
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
single/double-sided; a masked cluster of radius <= 1 m also marches the
terrain toward the sun once and packs the result into its entry's top 8
bits, which mesh.frag uses instead of its per-fragment march) -> atmosphere LUTs
(transmittance + multi-scattering only when the Atmosphere changes;
sky-view, aerial perspective every frame) -> weather (weather_map.comp:
512^2 over 60 km around the camera, texel-snapped; then the cloud shadow
map, cloud_shadow.comp: a Beer shadow map, 1024^2 over 30 km across the
sun, orthographic along it, texel-snapped in its frame, 32 steps per
texel across the layer, only for lines that reach a receiver within ~15
km of the camera, every other row per frame (alternating) once a full
map exists, and cloud_shadow_blur.comp, a bilateral 5x5 of its
extinction and optical depth; then sky_capture.comp: a coarse cloud march
from the camera into a 64^2 octahedral map (cloud luminance + haze,
transmittance; one texel of every 2x2 block per frame, all when the
history resets); then sky_irradiance.comp: the sky-view LUT with the capture's
clouds over it integrated into the scene's ambient irradiance, and the
LUT alone into the clear-sky irradiance that lights the clouds) -> early scene
pass into HDR RGBA16F + RG16F motion + R8 transparency mask + the Ray
Reconstruction guides (RGBA8 diffuse albedo, RGBA8 specular albedo, RGBA16F
world normal + roughness; null views unless the upscaler wants them) + RGBA16F sun light (the
sun's exposed radiance, for contact shadows) + D32 depth
(sky fullscreen, then the terrain (terrain.mesh, 16 workgroups of 8x8 quads per node, so hills
reach the depth pyramid; not drawn again late), then
vkCmdDrawMeshTasksIndirectEXT per list, mesh.mesh, one workgroup per
cluster; then the far field pass: voxel.mesh emits each far cell's
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
late scene pass (load; late lists, then blended instances CPU-sorted back
to front, mesh.vert) -> clouds (clouds_march.comp at quarter
resolution, half inside the layer or within 250 m of it: luminance with
haze + transmittance, mean cloud depth d, stored -1 - d where scene
geometry cut the march; the cirrus sheet (`cirrusLayer`) behind it on
sky pixels; clouds_resolve.comp accumulates it into a per-resolution history
reprojected by cloud depth + wind; clouds_composite.comp upsamples it
bilaterally on scene depth over HDR (as storage, GENERAL), writes
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
`ExposureState` exposure / previousExposure), then sharpen.comp, which
writes `output` from it (CAS; a copy at sharpness 0, today's setting),
both inside the `aa` pass)
-> bloom (bloom.comp, glare: 13-tap downsamples of `output` into a
half-display RGBA16F mip chain down to ~1/128, tent upsamples mixing each
coarser level in by `scatter`, then `output` = mix(output, level 0,
`intensity`), in place; energy-conserving, before the exposure meter so
it meters the glare as the eye does; `Renderer::setBloom`, intensity 0
skips it) -> luminance histogram + exposure adaptation (compute) -> grade
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
  sun-disk sample and the cloud march jitter use a PCG3D hash of (pixel,
  frameIndex) (`pcg3d`, common.glsl) instead of interleaved gradient noise.
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
  (push constants `CloudConstants`: scene, historyValid, inLayer).
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
  Parallax occlusion mapping (`parallaxHit` in mesh.frag) marches the
  dominant ground layer's height (`TerrainLayer::heightScale`, meters,
  from the Megascans JSON) and shifts every layer's sample position
  (`terrainParallax`) by the same world offset; depth, motion vectors and
  the RR guides stay the raster surface's.
- Wind: one global wind, owned by `Weather` (the game updates it with the
  absolute game time, then calls `Renderer::setWeather` before every
  `draw`). `WindState` is the 10 m wind (direction it blows toward, xz);
  other altitudes follow the power-law profile and veer
  (`windVelocityAt`, wind.glsl `windVelocity`). Anything carried by the
  wind scrolls by `windOffset` (the time integral of the velocity, never
  velocity x time, so speed changes don't jump), and computes motion with
  `scene.wind` and `scene.previousWind`. Tileable noise scrolls by an
  offset reduced modulo its period in double on the CPU
  (`Weather::windOffsetModulo`; clouds: `scene.clouds.windOffset` at
  3 km, modulo 2^18 m, which every cloud noise period divides), so float
  precision doesn't degrade with play time. Cloud reprojection and motion
  use `scene.clouds.windDisplacement` (the move since last frame, from
  offsets modulo 4096 m differenced in double), never a difference of
  the wrapped offsets. The weather at a position comes
  from `Weather::stateAt` / `weatherAt` (the history rule), never from
  the current state directly.
- Any struct in `gpu_types.h` marked "must match" mirrors a declaration in
  `shaders/common.glsl`, `post.glsl`, `exposure.glsl` or
  `clouds_temporal.glsl` (or the shader it
  names: `GpuPlacementLayer`/`PlacementConstants` in placement.comp,
  `ScanConstants` in scan.comp, `BloomConstants` in bloom.comp, `TlasFillConstants` in tlas_fill.comp,
  `GpuGrade`/`GradeConstants` in grade_lut.comp; `PlacedInstance` in placement.h); change
  both together (static_asserts check the sizes, and the std430 offsets of
  `SceneData`'s struct members: a struct with vec3 or 64-bit members aligns
  to 16 or 8 in GLSL, so give the C++ member `alignas` to match). `placement.glsl` mirrors
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
  structure builds are ignored with `DriverLeakScope` (gpu_types.h; dlss.cpp
  has its own copy for NGX calls). Don't
  use DWARF unwinding to chase them; it makes startup 6x slower.
- DLSS: NGX's device extension VK_NVX_binary_import makes the driver start
  CUDA, which fails device creation under ASan unless
  `protect_shadow_gap=0` (set by `__asan_default_options` in dlss.cpp; it
  applies to the whole Debug game binary when the module is linked). The
  SDK is a 684 MB commit tarball fetched once per build directory
  (`-DFETCHCONTENT_SOURCE_DIR_DLSS_SDK=<other build>/_deps/dlss_sdk-src`
  reuses one).
  NGX clears the upscaler output on reset, so `output` has TRANSFER_DST.
  The Renderer calls `UpscalerProvider::shutdown` (NGX shutdown) before it
  destroys the device; the provider outlives the Renderer. Credits: the
  game prints NVIDIA's attribution at startup and ships `credits.txt`.
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
  (needs `jq`); once the project is built it also cooks every model, and,
  given the Megascans downloads directory, the terrain layers
  (`assets/terrain/<id>/*.ktx2`; without them the terrain is procedural). Loading the Poly Haven
  models prints UBSan shift warnings from inside MikkTSpace (third-party
  code, not ours).
- Megascans trees, ground surfaces, the Fab cliff and their import tools:
  `docs/assets.md`. The rock placement layer and the terrain material's
  rock blend share `kRockNormalY` (renderer.h); the forest layers and the
  material's forest floor share the forest mask (`kMaskFrequency`...,
  main.cpp; `forestMask` in mesh.frag runs placement.glsl's mask).
- Dither patterns are cached in `assets/placement-cache/dither-<layer
  id>-<points>.bin` (`cachedDitherPattern`, placement.h; the game passes
  the directory to `setPlacement`): header = magic, format version, seed,
  count, kCandidatesPerPoint, kMaxCandidates; any mismatch or a short file
  rebuilds and rewrites it (an unwritable cache only warns). Bump
  `kPatternCacheVersion` (placement.cpp) when `makeDitherPattern`'s output
  changes for the same constants (e.g. `placementHash`). Warm startup
  spends ~0.1 s there (Release ~10 s and Debug ~59 s cold).
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
