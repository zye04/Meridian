# Meridian

C++20 / Vulkan 1.3 game engine (`engine/`, static library `meridian`) plus
the game built on it (`game/`). Goal: photorealistic, first-person, open
world, grand scale. Sun shadows are raster (cached cascaded shadow maps);
hardware ray tracing is still required (`--shadows rt` A/B, the far-field
bake, `--far-shadows tlas`). The setting is open,
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
- `docs/reference/cli.md`: every game flag, controls, overlays (G grade
  panel, F3/F4 perf), benchmark and route, reference path tracer, weather,
  AA, GI, shadows, motion gates, and the named camera views. Read it
  before adding or using a flag.
- `docs/reference/architecture.md`: source layout, the frame in pass order
  (renderer_frame.cpp), scene attachments, GI buffers, GPU-driven scene
  and instance ids, descriptor sets, terrain quadtree. Read the part a
  change touches.

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
- Keep this file, `docs/reference/*.md` and `docs/decisions.md` current in
  the same commit as the change they describe (a new flag gets a line in
  `docs/reference/cli.md`, a new pass one in its frame order).

## Commands

```bash
cmake -S . -B build && cmake --build build      # Debug: ASan+UBSan, validation + sync validation
ctest --test-dir build                           # or ./build/tests/tests
# the build uses ccache and mold automatically when installed (no setup needed)
flock /tmp/meridian-gpu.lock ./build/game/game   # interactive (WASD, mouse, Esc; F walk/fly)
./tools/fetch-sample-assets.sh                   # test models into assets/; also the Napf terrain (225 tiles, needs magick)
./tools/fetch-relief-patch.sh                    # 4x4 km Napf patch at 0.5 m and 2 m (relief-{0.5,2}.height; view (x, z) on napf.height = (x - 500, z - 500))
python3 -I tools/image-gates.py laplacian motion.png   # laplacian|ratio|shimmer|ssim|diff
tools/motion-gates.sh build-release out-dir      # motion/shimmer/SSIM gate table, GPU passes
tools/motion-clarity.sh build-release out-dir    # in-motion jitter (frame 120 vs 121, same pose) + smear vs still at user speeds
./build/tools/cook/meridian-cook in.gltf assets/Name   # PNG/JPEG -> KTX2 glTF (bakes a lone node's rotation, scale + vertical offset)
./build/tools/cook/meridian-cook --texture in.jpg out.ktx2 color|data|normal
./build/tools/cook/meridian-cook --stats a.ktx2 a.png   # mean linear colour (alpha-weighted) as the renderer transcodes it vs the source
./build/tools/cook/meridian-cook --heightfield in.f32 W H SPACING out.height   # float32 m -> 16-bit .height
./build/tools/cook/meridian-cook --geo in.height out.geo RATIO [LATITUDE]   # geo channels (tools/cook/geo_channels.h) beside the .height; the game loads x.geo with x.height
./build/game/game --terrain assets/terrain/relief-0.5.height --score-forest assets/terrain/relief-forest-truth.bin   # forest rule vs swissSURFACE3D canopy: kappa, F1
cmake -S . -B build-brute -DMERIDIAN_TERRAIN_MARCH_BRUTE_FORCE=ON   # A/B: terrain shadows by a 1 m linear march
cmake -S . -B build-off -DMERIDIAN_DLSS=OFF      # licence-clean build: no NVIDIA SDK, TAA only
python3 -I tools/region-luminance.py in.pfm X0 Y0 X1 Y1 [...]   # mean luminance per pixel rect (top-left origin, end exclusive) + ratio to the first
python3 -I tools/reference-compare.py raster.pfm reference.pfm --ev EV_RASTER EV_REF [--regions FILE --view NAME] [X0 Y0 X1 Y1 ...]   # raster vs --reference: per-region luminance ratio + rgb/G (4x4 grid by default), exit 1 on NaN/Inf
python3 -I tools/resemblance.py ours.png ref.png --regions docs/superpowers/research/matched-views/regions.json --view NAME   # OKLCh region stats, ours vs reference
tools/cloud-compare.sh build-release out-dir     # cloud look views beside the reference photos + measured targets (tools/cloud-metrics.py)
```

Every run of the game binary (interactive, capture, perf; Debug or
Release) goes through `flock /tmp/meridian-gpu.lock`, because two Claude
instances may render in parallel and GPU timings would collide. Hold it
only for the run, never for builds. Examples below omit the prefix.

Automated visual check (deterministic, ~4 s, exit code 1 on any Vulkan
validation error; all flags and views: `docs/reference/cli.md`):

```bash
./build/game/game --camera X Y Z YAW PITCH [--trees] [--placement off] --capture out.png [--frames N]
```

Start view `1119 1355.9 -431 -1.55 -0.1` (ridge top looking at the Napf
summit); `--placement off` skips rocks, plants and trees for captures that
don't need them (Release ~11 s startup instead of ~18 s); `--trees` brings
the trees back (hidden by default). Capture runs print validation errors,
`GPU passes:` (per-pass ms) and, in Debug, the placement checks and hash.

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
- Data layouts (scene attachments, GI buffers, instance ids, descriptor
  sets, terrain quadtree): `docs/reference/architecture.md`.

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
  rock blend share `kRockNormalY` (renderer.h). Placement layers are
  causal (spec `2026-11-23-geo-channels-causal-placement-design.md`):
  rules read the `.geo` channels (`DensityRule::channels`) and an earlier
  layer's density (`PlacementLayer::parent`); without a `.geo` the game
  skips channel layers. The forest floor follows the forest layer
  (`TerrainMaterial::forestLayerId`): setPlacement bakes its density per
  geo texel (placement.comp `PLACEMENT_FOREST`), surface.glsl
  `forestDensity` reads it.
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
