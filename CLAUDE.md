# Meridian

C++20 / Vulkan 1.3 game engine (`engine/`, static library `meridian`) plus
the game built on it (`game/`). Goal: photorealistic, first-person, open
world, grand scale. Hardware ray tracing is required. The setting is open,
so systems stay parametric (e.g. the atmosphere supports other planets).
Technique choices and their sources are in `docs/decisions.md`; read it
before re-researching anything.

## Working agreement

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
./build/game/game                                # interactive (WASD, mouse, Esc)
./tools/fetch-sample-assets.sh                   # test models into assets/
./build/tools/cook/meridian-cook in.gltf assets/Name   # PNG/JPEG -> KTX2 glTF
./build/tools/cook/meridian-cook --texture in.jpg out.ktx2 color|data|normal
```

Automated visual check (deterministic, ~4 s, exit code 1 on any Vulkan
validation error):

```bash
./build/game/game --camera X Y Z YAW PITCH [--sun X Y Z] --capture out.png [--frames N]
```

Motion sharpness: add `--camera-velocity X Y Z` (m/s, fixed 1/60 s steps,
ending at `--camera`) and compare with the still capture of the same view:
`magick out.png -colorspace gray -morphology Convolve Laplacian:0 -format
"%[fx:standard_deviation]\n" info:` (higher = sharper).

Useful views: default start `0 300 700 0 -0.35`; sample models
`-1.6 299.2 700 -0.35 -0.12`; sunset `--sun 0.9 0.05 -0.4` with yaw 1.15.

## Testing policy (usage matters)

- Always build. Run tests when logic they cover changes.
- Capture only when rendered output can change, with the fewest views that
  prove the change. The capture run's output already reports validation
  errors, leaks and GPU time; don't do separate runs for those.
- GPU ms are only comparable at the same window size. Hyprland tiling
  changes the size, so the capture resolution varies between runs.
- Interactive runs pick up the user's mouse; use capture runs for checks.
- Two captures of the same view never match bit for bit (per-frame noise in
  soft shadows and TAA); compare the mean difference (~0.02/255), not exact
  pixels.
- Prefer unit tests for math (e.g. frustum planes in tests/mat4_test.cpp)
  over visual checks.

## Layout

```
engine/include/meridian/   public API: renderer.h (Renderer: uploadMesh,
                           createMaterial, uploadTexture, setTerrainMaterial,
                           addInstance, draw; Lighting, Atmosphere,
                           MaterialData, TerrainMaterial), camera.h, mesh.h,
                           model.h (loadGltf), texture.h (loadKtx2),
                           terrain.h, window.h, math/{vec3,mat4}.h
engine/src/render/         renderer internals:
  gpu_types.h              constants + structs mirroring GLSL layouts
  vulkan_util.*            barriers, shader loading, pipeline builders
  renderer_impl.h          Renderer::Impl + submitNow()
  renderer_setup.cpp       init/teardown: swapchain, frames, descriptors,
                           pipelines, atmosphere, materials
  renderer_resources.cpp   buffers, images, BLAS, texture/material/mesh upload
  renderer_frame.cpp       per-frame: instance upload, TLAS, GPU culling,
                           all passes in draw()
engine/src/                model.cpp (glTF), texture.cpp (KTX2),
                           terrain.cpp, window.cpp
engine/shaders/            GLSL; *.glsl are includes (see below)
game/src/main.cpp          scene setup, fly camera, CLI options
tools/cook/                meridian-cook
tests/                     doctest unit tests
docs/decisions.md          technique decisions with sources
```

## Frame (renderer_frame.cpp, in order)

Instance table upload (when changed) -> TLAS build over all instances ->
GPU frustum culling (cull.comp fills 4 indirect draw lists: opaque/masked x
single/double-sided) -> atmosphere LUTs (transmittance + multi-scattering only when the
Atmosphere changes; sky-view, aerial perspective, sky irradiance every
frame) -> scene pass into HDR RGBA16F + RG16F motion + D32 depth (sky
fullscreen, then vkCmdDrawIndexedIndirectCount per list, then blended
instances CPU-sorted back to front) -> TAA resolve
(compute) -> luminance histogram + exposure adaptation (compute) -> AgX
tonemap into the swapchain. 2 frames in flight; the HDR/depth/LUT/history
images are shared between them, and barriers account for that.

## Conventions

- Units: meters; y = 0 is sea level. The atmosphere works in km. Light in
  lux (sun) and cd/m^2; shading is pre-exposed with last frame's exposure
  (GPU buffer) so HDR values stay in FP16 range.
- Space: right-handed, Y up, camera looks down -Z. Reverse-Z with an infinite
  far plane (depth 1 = near; compare GREATER_OR_EQUAL; clear to 0).
  Mat4 is column-major `m[column][row]`.
- Motion vectors: UV units, `previous - current` (the DLSS convention).
- GPU-driven scene: all meshes live in shared vertex/index pools (bump
  allocated); `MeshInfo` and `InstanceData` tables are on the GPU;
  `gl_InstanceIndex` = instance index (firstInstance), and the TLAS custom
  index is the same. Push constants carry only the scene buffer address;
  everything else is reached through it (buffer device addresses).
  Descriptor sets: scene layout set 0 = atmosphere LUTs, set 1 = DFG LUT
  + bindless `textures[]`; post layout set 0 = HDR/motion/history (two
  ping-pong sets).
- Any struct in `gpu_types.h` marked "must match" mirrors a declaration in
  `shaders/common.glsl`, `post.glsl` or `exposure.glsl`; change both
  together (static_asserts check the sizes).
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
- `assets/` is git-ignored. Re-create it with `tools/fetch-sample-assets.sh`
  (needs `jq`); once the project is built it also cooks every model and the
  terrain layers (`assets/terrain/<id>/*.ktx2`). Loading the Poly Haven
  models prints UBSan shift warnings from inside MikkTSpace (third-party
  code, not ours).
- Known gaps: masked-geometry RT shadows are not visually verified yet; TAA
  history isn't rescaled on exposure change; the Renderer constructor
  leaks partial Vulkan state if it throws; terrain is one 2 km mesh with no
  LOD or streaming.
