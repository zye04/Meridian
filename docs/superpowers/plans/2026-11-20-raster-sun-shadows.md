# Raster Sun Shadows Implementation Plan

**Status:** done; Task 5 flipped the default on 2026-11-21 (decisions.md "Raster sun shadows are the default").

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** sun shadows of objects from cached, texel-snapped cascaded shadow
maps rendered through the existing GPU-driven cluster pipeline, replacing
the per-pixel TLAS ray (kept as `--shadows rt`).

**Architecture:** a pure CPU module computes the cascade windows, light
matrices and which cascades render this frame. A stale cascade gets its
own `SceneData` copy (flagged `shadowView`, orthographic LOD) and runs the
existing cell cull -> cull -> traverse -> cluster cull chain in a shadow
mode into its own counters and draw lists, then a depth-only mesh-shader
pass. mesh.frag's sun visibility multiplies the terrain march by a 3x3
tent PCF lookup into the cascades.

**Tech Stack:** C++20, Vulkan 1.3 mesh shaders, GLSL, doctest.

**Spec:** `docs/superpowers/specs/2026-11-20-raster-sun-shadows-design.md`

## Global Constraints

- `kShadowCascades = 5`, `R_i = 20 * 4^i` m, `H_i = 1.25 R_i + margin`, `kShadowMapSize = 2048`, D32_SFLOAT, reverse depth (1 = sunward), compare GREATER_OR_EQUAL, clear 0.
- Re-centre when the camera's offset from a cascade's centre along u, v or d exceeds `0.5 (H_i - R_i)`; at most one cascade per frame except on full invalidation (first frame, sun changed, instances/placement changed, switched to raster).
- `--shadows raster|rt`, default raster. The TLAS, BLAS and ray-query code stay.
- Terrain march (`terrainOccludes`, cluster_cull's per-cluster sun), cloud shadow, contact shadows and the far branch of `traceSunVisibility` are unchanged.
- Streamed layers and blended meshes cast no cascade shadows (parity with the TLAS).
- Any `gpu_types.h` struct marked "must match" changes together with `common.glsl` (static_asserts on sizes/offsets). Add new SceneData members at the end.
- Every game run: `flock /tmp/meridian-gpu.lock ...`. Captures at 2560x1440 fullscreen. Temp files in `.superpowers/`.
- Commits: no AI attribution, no Co-Authored-By trailer. One feature per commit.
- Don't touch `clouds_*.comp` (cloud lane) or far-field files beyond the `traceSunVisibility` hook.

## Review Focus

- Sun at or below the horizon (elevation <= 0, `--sun 0.9 -0.05 -0.4`): no NaN matrices, nothing rendered, lookups return lit (the sun term is already 0). Test in Task 1 (`ShadowCascades: sun below horizon`).
- Sun straight overhead (direction = +y): the light basis must not degenerate. Test in Task 1 (`light basis: zenith sun`).
- Camera far from the origin (`7000 1288.2 7000`, the crop corner): snapping in double, no float precision shimmer. Test in Task 1 (`snapping far from origin`).
- Camera moving faster than re-centring keeps up (fly x5, 150 m/s): receivers outside a cascade's rendered window fall back to the next coarser one, no black or unshadowed band. Covered by the lookup rule in Task 4 and the fast-fly capture in Task 5.
- `--shadows rt` must render exactly as before (A/B): Task 4 verifies an rt capture against main's within capture noise.

---

### Task 1: Cascade math module and orthographic LOD (CPU, tested)

**Files:**
- Create: `engine/include/meridian/shadow_cascades.h`, `engine/src/shadow_cascades.cpp`
- Modify: `engine/include/meridian/cluster.h`, `engine/src/cluster.cpp` (orthographic LOD), `engine/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/shadow_cascades_test.cpp`, `tests/cluster_test.cpp` (or wherever `selectClustersBvh` is tested)

**Interfaces:**
- Produces:
  - `constexpr uint32_t kShadowCascades = 5; constexpr uint32_t kShadowMapSize = 2048;`
  - `double cascadeRadius(uint32_t i)` (20 * 4^i), `double cascadeHalfSize(uint32_t i)` (1.25 R_i + 4 texels of filter margin, solved so the margin is in that cascade's texels).
  - `struct LightBasis { DVec3 u, v, d; };` `LightBasis lightBasis(Vec3 sunDirection)` (d = -sun, u = normalize(cross(d, up')) with up' = +z when |d.y| > 0.99 else +y; right-handed, double).
  - `struct CascadeWindow { DVec3 center; double halfSize, texel, depthNear, depthFar; };` (center: world, double; depthNear: distance from center toward the sun (positive) where the box starts; depthFar: distance away from the sun where it ends)
  - `CascadeWindow cascadeWindow(uint32_t i, const LightBasis &, DVec3 camera, double casterTop, Vec3 sunDirection)`: center = camera's (u, v) snapped to whole texels of cascade i, plus camera's d coordinate; depthFar = halfSize; depthNear = min(kShadowRayLength, (casterTop - (camera.y - halfSize)) / max(sinElevation, 0.02) + halfSize).
  - `Mat4 cascadeProjection(const CascadeWindow &, const LightBasis &)`: maps (world - center) to clip, x/y in [-1,1] over ±halfSize along u/v, z = 1 at depthNear sunward, 0 at depthFar (reverse depth).
  - `std::array<std::array<float,4>,6> cascadePlanes(const CascadeWindow &, const LightBasis &)`: inward planes relative to center (same convention as `SceneData::frustumPlanes`: dot(n, p) + w >= -radius inside).
  - `bool needsRecenter(uint32_t i, const CascadeWindow &rendered, const LightBasis &, DVec3 camera)`: |offset| along u, v or d > 0.5 (H - R).
  - `struct ShadowUpdateInput { bool fullInvalidation; std::array<bool, kShadowCascades> recenter; };` `std::array<bool, kShadowCascades> cascadesToRender(const ShadowUpdateInput &)`: all on full invalidation, else only the finest with recenter set.
  - `float projectedErrorPixels(...)` / `lodVisible(...)` / `selectClustersBvh(...)` gain the orthographic case: add a trailing `float orthoPixelsPerMeter = 0.0f` parameter; when > 0 the projected error is `error * scale * orthoPixelsPerMeter` (distance, projY, screenHeight ignored).
  - If `DVec3` doesn't exist in `math/vec3.h`, define it in shadow_cascades.h (x, y, z doubles, the few ops needed).
- Consumes: `Vec3`, `Mat4` (math/), `kShadowRayLength` value 20000 (duplicate as a C++ constant if none exists, comment "must match common.glsl").

- [ ] **Step 1: Write failing tests** in `tests/shadow_cascades_test.cpp`:
  - `cascade radii`: `cascadeRadius(0) == 20`, `cascadeRadius(4) == 5120`; `cascadeHalfSize(i) > 1.25 * cascadeRadius(i)`.
  - `light basis orthonormal`: for sun (0.45, 0.85, 0.25), (0.9, 0.05, -0.4) normalized, and `light basis: zenith sun` (0, 1, 0): |u|=|v|=|d|=1 within 1e-12, pairwise dots < 1e-12, d == -sun.
  - `snapping`: two cameras 0.3 texel apart in u produce the same center (u, v) coordinates; moving one full texel moves the center by exactly one texel (1e-9). `snapping far from origin`: same at camera (7000, 1288.2, 7000).
  - `projection maps the box`: corners of the box (center ± halfSize u ± halfSize v, depthNear sunward / depthFar) project to x,y = ±1 and z = 1 / 0 (1e-5).
  - `planes contain the box`: points inside the box are inside all 6 planes; a point 1 m outside any face is outside that plane.
  - `recenter threshold`: camera at offset 0.49 (H-R) in u: false; 0.51: true.
  - `update policy`: full invalidation -> all true; recenter {false,true,true,false,false} -> only cascade 1.
  - `ShadowCascades: sun below horizon`: sun (0.9, -0.05, -0.4): `cascadeWindow` returns finite values (depthNear capped at 20000).
  - In the cluster tests: `selectClustersBvh orthographic equals lodVisible` over the existing test meshes at orthoPixelsPerMeter 2048/50 and 2048/12800.
- [ ] **Step 2:** `cmake --build build-release --target tests && ./build-release/tests/tests -tc='*cascade*,*light basis*,*snapping*,*projection*,*planes*,*recenter*,*policy*,*orthographic*'` -> fails to compile/link.
- [ ] **Step 3:** implement the module and the orthographic LOD parameter.
- [ ] **Step 4:** run the full `./build-release/tests/tests` -> all pass.
- [ ] **Step 5:** commit "Cascade math for raster sun shadows" (message: what the module computes and why snapped, cached cascades).

### Task 2: Shadow mode in the GPU cull chain

**Files:**
- Modify: `engine/src/render/gpu_types.h` (SceneData), `engine/shaders/common.glsl` (SceneBuffer, `curvatureDrop`), `engine/shaders/cull.glsl` (`projectedError`), `engine/shaders/cell_cull.comp`, `engine/shaders/cull.comp`, `engine/shaders/cluster_traverse.comp`, `engine/shaders/cluster_cull.comp`

**Interfaces:**
- Produces (SceneData, appended at the end with matching GLSL): `uint32_t shadowView` (1 in a cascade's copy), `float orthoPixelsPerMeter` (0 = perspective), `uint32_t shadowMode` (0 raster, 1 rt; used by Task 4), plus `GpuSunShadows sunShadows` (Task 4 fills it; define it now): per cascade `{ float origin[3] (center relative to the camera, float); float halfSize; float texel; float depthNear; float depthFar; uint32_t texture (bindless index); float radius (R_i); uint32_t valid; }` plus the shared `u`, `v`, `d` (vec3). static_assert the new offsets.
- Shader behaviour when `scene.shadowView != 0`:
  - `curvatureDrop` returns 0.
  - `projectedError` returns `error * orthoPixelsPerMeter` when it is > 0; `seedTraversal`'s `finest` likewise uses `level.minError * scale * orthoPixelsPerMeter`.
  - cell_cull.comp: only `cull.phase == 1` does anything; grid cells: box/draw-distance test only (no far switch, no far list, no visibility read/write); streamed records: skipped; hand-placed chunks: emitted.
  - cull.comp: phase 1 path with occlusion off, no `visibility` write, `processed` = false.
  - cluster_traverse.comp / cluster_cull.comp: no occlusion, no pushes to phase 1 retest, no terrain-sun code (entry.x carries only the instance), list = bucket (phase-0 lists of the shadow draw-list buffer).
- Consumes: Task 1's orthographic definition (must match `projectedErrorPixels`).

- [ ] **Step 1:** add the fields and static_asserts; build: `cmake --build build-release` -> compiles, `./build-release/tests/tests` passes.
- [ ] **Step 2:** implement the shader branches (shadow mode only active when the flag is set; the main view's behaviour unchanged).
- [ ] **Step 3:** verify the main view is unchanged: capture the start view (`--placement off` and default placement) before and after; `python3 -I tools/image-gates.py diff a.png b.png` mean diff ~1e-6/255 level (capture noise) and identical `drawn instances/clusters` lines.
- [ ] **Step 4:** commit "Shadow mode for the GPU cull chain".

### Task 3: Cascade rendering pass

**Files:**
- Create: `engine/shaders/shadow.mesh`, `engine/shaders/shadow_masked.frag`, `engine/src/render/renderer_shadows.cpp`
- Modify: `engine/CMakeLists.txt` (`add_shader`, source), `engine/src/render/renderer_impl.h`, `engine/src/render/renderer_setup.cpp` (create/destroy), `engine/src/render/renderer_frame.cpp` (call + `endPass("sun shadows")`), `engine/include/meridian/renderer.h` (`enum class ShadowMode { Raster, Rt }; void Renderer::setShadowMode(ShadowMode)`), `game/src/main.cpp` (`--shadows raster|rt`)

**Interfaces:**
- Consumes: Task 1 (`lightBasis`, `cascadeWindow`, `cascadeProjection`, `cascadePlanes`, `needsRecenter`, `cascadesToRender`, orthographic LOD), Task 2 (SceneData fields, shadow mode).
- Produces: `void Renderer::Impl::renderSunShadows(VkCommandBuffer, FrameData &, const SceneData &frameScene)` that, before the main view's draw-counter reset, renders the stale cascades and fills `frameScene.sunShadows` (valid cascades only; `origin` = center - camera); `impl->shadowMode`.
- Resources: 5 D32_SFLOAT 2048^2 images (DEPTH_STENCIL_ATTACHMENT | SAMPLED), each in the bindless table (depth aspect view, read with `textureGather` through a nearest/clamp sampler); a shadow `DrawCounters` buffer; shadow draw lists (`kDrawBuckets * kMaxDrawnClusters` entries); one SceneData buffer per cascade per frame in flight (host-visible like `frame.sceneBuffer`).
- Per cascade rendered: copy the frame's SceneData, set `cameraPosition` = center (float), `frustumPlanes` = `cascadePlanes`, `viewProjection` = `cascadeProjection`, `drawLists` = shadow lists, `shadowView = 1`, `orthoPixelsPerMeter = 1 / texel`, `lodErrorPixels = 1`; reset the shadow counters; `cullCells`/`cullInstances`/`traverseClusters`/`cullClusters` with phase 1 and the shadow counters (generalise `cullConstants` to take the counters address and scene address; no readback copy for the shadow counters); then a dynamic-rendering pass into the cascade's image: clear 0, two pipelines (opaque: no fragment shader; masked: `shadow_masked.frag`), `VK_CULL_MODE_NONE`, depth compare GREATER_OR_EQUAL, depth bias enabled (dynamic, start constant 0, slope -1.5 in reverse depth; Task 5 tunes), all four bucket lists via `vkCmdDrawMeshTasksIndirectEXT`; barrier to fragment-shader read.
- Change tracking: last rendered sun direction, an instances/placement generation counter the existing upload path bumps, shadow mode transitions.
- With `ShadowMode::Rt` nothing renders and `sunShadows` cascades are invalid.

- [ ] **Step 1:** shaders + pipelines + images + bindless registration; build Debug and Release.
- [ ] **Step 2:** the pass and the CLI flag; capture the start view in Release with `--shadows raster` and dump nothing new yet (lookup comes in Task 4): the run is validation-clean in Debug (`./build/game/game --placement off --camera 1119 1355.9 -431 -1.55 -0.25 --capture .superpowers/t3.png`, exit 0) and the `GPU passes:` line shows `sun shadows` > 0 on the capture's first frame (print the frame-1 value once at capture end if the line only reports the last frame; one line `sun shadows: N cascades rendered, X ms (frame 1)` is enough).
- [ ] **Step 3:** temporary verification (not committed): dump cascade 0 and 2 to PNG via a scratch readback or RenderDoc-free path of your choice and look at them: rocks/plants visible as depth silhouettes, no terrain. Delete the scratch code.
- [ ] **Step 4:** commit "Render the sun shadow cascades through the cluster cull chain".

### Task 4: Lookup and the raster/rt switch

**Files:**
- Modify: `engine/shaders/lighting.glsl` (`cascadeVisibility`, `traceSunVisibility`), `engine/shaders/mesh.frag` only if the call needs the normal (pass the receiver normal through as the far lane's branch does), `engine/src/render/renderer_frame.cpp` (`scene.shadowMode`)

**Interfaces:**
- Consumes: `scene.sunShadows`, `scene.shadowMode` (Task 2/3).
- Produces: `float cascadeVisibility(vec3 relativeToCamera, vec3 normal)` in lighting.glsl.
- Rules (spec "Lookup"): finest valid cascade with `distance < R_i` whose window contains the receiver plus 2 texels; linear blend with i+1 over the outer 15% of R_i; beyond `R_4` or no window: 1. Receiver offset along the geometric normal `kShadowNormalOffset * texel_i` (start 1.5) and depth bias `kShadowDepthBias * texel_i` (start 1.0). 3x3 tent-weighted bilinear PCF from 9 `textureGather`s. Work in camera-relative float coordinates (`origin` is camera-relative).
- `traceSunVisibility` in raster mode: no ray query; `cascadeVisibility` x terrain march; the far branch untouched.

- [ ] **Step 1:** implement; Release build.
- [ ] **Step 2:** rt unchanged: capture start view with `--shadows rt` and compare with Task 2's main capture: mean diff at capture-noise level.
- [ ] **Step 3:** raster captures of the start view, low sun (`1119 1356 -431 1.57 -0.02 --sun 0.9 0.05 -0.4`), sunset (`1119 1500 -431 3.14 -0.15 --sun 0.9 0.05 -0.4`), the rock close-up (`1944.3 953.4 -1923.9 -1.55 -0.4`) and forest edge with `--trees`; side by side with rt (`magick a.png b.png +append`): shadows present where rt has them, no acne, no missing caster classes. Report any visible defect instead of tuning (Task 5 tunes).
- [ ] **Step 4:** Debug run validation-clean; commit "Raster sun shadows from cached cascades (default), --shadows rt for A/B".

### Task 5: Tuning, gates, docs

**Files:**
- Modify: constants from Tasks 3-4, `CLAUDE.md` (intro "Hardware ray tracing is required", frame order, passes list, `--shadows` flag, layout entries for the new files), `docs/decisions.md` (new entry "Raster sun shadows: cached cascades (2026-11-20)", referencing "Shadows"), `docs/backlog.md` (streamed layers cast no cascade shadows; TLAS/BLAS removal decision; PCSS if the fixed penumbra is too uniform)

- [ ] **Step 1:** tune bias/offset at the five views of Task 4 until acne and peter-panning are gone; then the cascade-band check (pan view at pitch -0.25 across 16-25 m and 64-80 m distances).
- [ ] **Step 2:** gates, raster vs rt, same build: `tools/motion-gates.sh build-release .superpowers/gates-raster --shadows raster` and `... --shadows rt`; slow-strafe shimmer (`--camera-velocity 0 0 0.5 --capture-sequence 1 --frames 60` at the start view and the forest edge `--trees`; mean of `image-gates.py shimmer` over consecutive pairs); fast fly (`--camera-velocity -150 0 0 --frames 120`) look check; GPU ms at midday, low sun, sunset, forest edge and overview (`--trees`): steady frame and the frame-1 `sun shadows` cost.
- [ ] **Step 3:** docs as listed; commit "Document raster sun shadows" folded into the last code commit if only docs changed with it.
