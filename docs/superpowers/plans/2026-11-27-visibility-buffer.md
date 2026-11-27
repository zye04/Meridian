# Visibility Buffer Implementation Plan

Status: draft (2026-11-27), waiting for the director's read. Branch
`visibility-buffer` from main a43f627. Stage 0 is done (go):
`docs/superpowers/research/2026-11-27-vb-stage0.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shade opaque and alpha-masked cluster triangles and voxel quads once per visible pixel: an id + depth scene pass, then one fullscreen resolve, with the forward look unchanged.

**Architecture:** The scene pass gains attachment 7, an `RG32UI` id target. The cluster lists and voxel quads draw through visibility pipelines that write only depth and the id (colour write mask 0 on attachments 0-6). Terrain and the far field stay forward and write id 0. After the late lists a fullscreen fragment pass reads the id, rebuilds the triangle or voxel, and shades it with the same GLSL functions the forward shaders use, writing attachments 0, 1, 3, 4, 5, 6. Blended surfaces draw after it.

**Tech Stack:** C++20, Vulkan 1.3 (dynamic rendering, mesh shaders), GLSL via glslc, ctest.

**Spec:** `docs/superpowers/specs/2026-11-27-visibility-buffer-design.md`. Read it with this plan; the corrections below override it where they disagree.

## Spec premises found false (corrections this plan builds on)

1. **The id layout does not fit.** The spec puts kind (2 bits) and cluster index and triangle (7 bits) in `y`. The cluster index is bounded by `kMaxClusterIndex = 1 << 26` (gpu_types.h:71; work items pack it in 26 bits), so `y` would need 2 + 26 + 7 = 35 bits. Correction, still 64 bits:
   - The terrain sun code has three values, not 256: 0 (shadowed), 1 (lit), 255 (`kTerrainSunPerFragment`, common.glsl:201). Two bits hold it (0, 1, 3 = per fragment).
   - `x` = instance id (24 bits) | triangle index << 24 (7 bits) | 1 bit spare.
   - `y` = cluster index (26 bits) | terrain sun << 26 (2 bits) | 2 spare | kind << 30 (2 bits: 0 none, 1 cluster triangle, 2 voxel).
   - Voxel: `x` = 0, `y` = the quad's index in its phase list (26 bits) | phase << 26 (1 bit) | kind 2 << 30.
2. **`kMaxVoxelQuads` is 55 870 000, not 24.66M** (gpu_types.h:1048). It still fits 26 bits (2^26 = 67 108 864), but the index is per phase: `voxelQuadListStart(list)` = base + phase x kMaxVoxelQuads. That is why the voxel id carries the phase bit.
3. **Guides 4 and 5 exist only under Ray Reconstruction.** Specular albedo and normal-roughness are null views unless the upscaler `wantsGuides()` (architecture.md "Scene attachments"). With the default DLAA they are discarded. The spec's guide gate (attachments 1, 3, 4, 5, 6) therefore runs with `--rr on`.
4. **The gates' tools are not on main.** There is no tool for the sparkle number (decisions.md 5833: white dots, L > 0.85, per 1e4 px), the tri/vox seam ratio, or dumping attachments 1, 4, 5, 6. `--capture-albedo` covers attachment 3 only. Task 3 adds them.
5. **Blended surfaces draw inside the late scene pass today** (renderer_frame.cpp:1432-1463, same `vkCmdBeginRendering`). The resolve must sit between the late lists and the blended draws, so the late rendering is split in two. The spec's diagram implies this but doesn't say it.

Smaller notes, not blocking:
- The spec's Wind section says stage 1 lands the shared position function; its stage list gives stage 1 only the derivatives. This plan does both in stage 1 (Task 2). It is a pure refactor and passes the same parity gate.
- "voxel_instance.mesh unchanged in what it emits" holds for positions only. The visibility variant must emit a per-primitive id, so it is a `VISIBILITY` variant of that file, not the same binary.
- Stage 0 found the mesh shader's per-vertex savings small: about 0 ms except 1.75 ms at the overview. Nothing in the plan depends on them.

## Global Constraints

- 2560x1440 Release for every gate number, with identical `Render WxH` lines between compared runs; every game run goes through `flock /tmp/meridian-gpu.lock`.
- `--visibility off|on` selects the path until stage 4 (a launch flag; the debug set stays forward). Default `off` until Task 12.
- Look parity (re-based by the director 2026-11-27 after T1): captures with placement are not repeatable (CLAUDE.md "Testing policy"; backlog "Placement captures not repeatable": start 0.58/255, edge 0.23, overview 0.45, close leaf 0.02 between two runs of one build). So every look gate has two parts:
  - **`--placement off` views:** exact (0.0000/255), or the difference explained by a bisect like T1's.
  - **Views with placement:** measured against a floor run (the same build twice, in the same session), reporting both figures. The bar is the gate's figure above the floor: forward-parity steps (stages 1 and 2 `off`) allow floor + 0.02/255; `--visibility on` against `off` allows floor + 0.3/255 at tree views.
  - Above either bar, or on any visible change, a whole-frame A/B sheet goes to the user.
- Cost: no view loses more than 0.3 ms. The forest views gain stage 0's projection within a third (edge 9.2 ms, overview 6.0 ms, so at least 6.1 and 4.0).
- VRAM: resident +28.1 MiB (2560 x 1440 x 8 B), peak not above main's.
- Debug: one short capture per stage with 0 validation errors; the debug views keep working, forward.
- Counters: 0 dropped by full cull lists at every view, cascades included; the cascades are unchanged.
- Commits carry no AI attribution trailer. Nothing reaches main without the director's merge call.
- CLAUDE.md "Conventions" apply: "must match" structs change on both sides with static_asserts, and comments explain only a non-obvious why.

## Review Focus

1. **Edge pixels of sub-pixel triangles:** a pixel whose centre the rasterizer gave to triangle T must get barycentrics inside T, within 1e-3, from the resolve's recomputed positions. The rasterizer and the resolve can disagree by FMA contraction. Covered by Task 4 (`precise`, the clamp counter) and gated in Task 8: clamped share below 1e-4 of resolved pixels.
2. **Double-sided leaves seen from the back:** facing must match the rasterizer's (`VK_FRONT_FACE_COUNTER_CLOCKWISE`, negative-determinant transforms included). Covered by the Task 4 unit test `facingMatchesWindingUnderMirror`, and in Task 8 by the normal guide compared off vs on at the forest edge.
3. **A pixel covered early by a cluster, then late by terrain:** cannot happen; terrain draws only early. **A pixel covered by a cluster early, then won by the far field:** must read id 0. Covered by the far field's id-0 write (Task 5) and checked in Task 8 at the overview (far cells present), with a whole-frame diff.
4. **Streamed plants mid-fade (`instance.fade < 1`):** the resolve must reach the same `loadInstance` answer and blend toward `terrainNormal()` with explicit gradients. The meadow view in the Task 8 look gate covers this, together with a `--camera-velocity` motion clip through the streamed ring (motion-gates).
5. **Exposure and debug toggles mid-run (F5 debug views with `--visibility on`):** swapping to the debug set and back must rebuild the id-target pipelines and leave no stale id. Covered by Task 5 Step 6: a `--capture-views` list alternating `--debug-view lod` and none.

---

## Stage 1: explicit derivatives and shared functions (forward only; mergeable alone)

### Task 1: Shared surface code takes explicit gradients

**Files:**
- Modify: `engine/shaders/surface.glsl` (SurfaceInput, `sampleTexture`, `terrainNormal`)
- Modify: `engine/shaders/lighting.glsl:148-151` (cascade taps)
- Modify: `engine/shaders/mesh.frag` (fills the new fields)
- Modify: `engine/shaders/reference.comp` (fills zero gradients if it fills `surfaceIn` fields by name)

**Interfaces:**
- Produces (as built, c03b527): `SurfaceInput` gains `vec2 uvDx, uvDy, terrainUvDx, terrainUvDy;` and `vec2 terrainNormalUv(vec3 worldPosition)`; the forward path fills them with `dFdxCoarse`/`dFdyCoarse` (the implicit LOD's derivatives), the resolve with analytic gradients. Planned form: `sampleTexture(uint index, vec2 uv)` under `SURFACE_FRAGMENT` calls `sampleGrad(index, uv, surfaceIn.uvDx, surfaceIn.uvDy)` (surface.glsl:132, already scales by `2^mipBias`), so it must move below `sampleGrad` or `sampleGrad` above it. `terrainNormal()` under `SURFACE_FRAGMENT` uses `textureGrad` with the uv gradients `surfaceIn.dpdx.xz / (spacing * size)` and `surfaceIn.dpdy.xz / (spacing * size)`.

- [x] **Step 1:** Change the cascade taps in `lighting.glsl` from `texture(shadowMaps[...], vec3(...))` to `textureLod(shadowMaps[...], vec3(...), 0.0)`. The maps have one mip, so this changes nothing visible, and the resolve then has no implicit LOD left in shared code.
- [x] **Step 2:** Add `uvDx`/`uvDy` and switch `sampleTexture` and `terrainNormal` as in Interfaces. In `mesh.frag` set `surfaceIn.uvDx = dFdx(inUv); surfaceIn.uvDy = dFdy(inUv);` beside `dpdx`. `mesh.frag`'s own `sampleTexture(material.diffuseTransmissionColorTexture, inUv)` then goes through the same path.
- [x] **Step 3: Verify no implicit LOD or derivative remains in shared code**
  Run: `grep -nE 'dFd|fwidth|\btexture\(' engine/shaders/{surface,material,lighting,terrain,atmosphere,common,gi}.glsl`
  Expected: no output.
- [x] **Step 4: Build, ctest, parity**
  Run: `cmake --build build-release && ctest --test-dir build-release`. Then capture main (a43f627, in `../meridian/build-release`) and this branch at the start view (`1119 1355.9 -431 -1.55 -0.1`), the forest edge (`600 1205.6 -400 -1.55 0.02`) and a close leaf view (the stage-0 r4 pose `1944.3 953.4 -1923.9 -1.2 -0.9`), all `--trees`, and run `python3 -I tools/image-gates.py diff main.png branch.png`.
  Done 2026-11-27 (c03b527): with `--placement off`, the terrain normal and cascade taps are exact. Model textures through `textureGrad` differ from implicit LOD by 0.169/255 at start; the director accepted that gap (option a).
- [ ] **Step 5: Commit** (`Shared surface code: explicit texture gradients, no implicit LOD`)

### Task 2: Shading and vertex functions shared between forward and resolve

**Files:**
- Create: `engine/shaders/cluster_vertex.glsl`: the cluster vertex transform, crown visibility and tuft height, moved out of `mesh.mesh`.
- Create: `engine/shaders/cluster_shading.glsl`: the non-terrain, non-debug body of `mesh.frag`'s `main`.
- Create: `engine/shaders/voxel_shading.glsl`: the body of `voxel_instance.frag`'s `main`, and the voxel quad decode from `voxel_instance.mesh`.
- Modify: `engine/shaders/mesh.mesh`, `mesh.frag`, `voxel_instance.mesh`, `voxel_instance.frag`.

**Interfaces (Produces, used by Tasks 5, 6 and 10):**
```glsl
// cluster_vertex.glsl
struct ClusterVertexOut { vec3 relative; vec4 clip; vec4 currentClip; vec4 previousClip;
                          vec3 normal; vec4 tangent; vec2 uv; float alpha;
                          float height; float crownVisibility; };
struct ClusterContext { LoadedInstance instance; ClusterInfo cluster; MeshInfo mesh;
                        bool hasCrown; MeshInfo crown; bool tuft; mat3 normalMatrix; };
ClusterContext loadClusterContext(SceneBuffer scene, uint instanceId, uint clusterIndex);
// Vertex i (0..cluster.vertexCount-1) of the cluster. `precise` on the position math:
// the resolve must reproduce gl_Position bit for bit.
ClusterVertexOut clusterVertex(SceneBuffer scene, ClusterContext c, uint i);
// cluster_shading.glsl
struct ClusterPixel { vec3 relative; vec3 relativeDx; vec3 relativeDy; vec3 normal;
                      vec3 geometricNormal; bool frontFacing; vec2 uv; vec2 uvDx; vec2 uvDy; vec2 terrainUvDx; vec2 terrainUvDy;
                      vec4 tangent; uint materialIndex; float lodError; float height;
                      vec3 tint; float streamFade; float crownVisibility; float terrainSun;
                      vec4 currentClip; vec4 previousClip; vec3 normalDx; vec3 normalDy; };
struct SceneOutputs { vec4 color; vec2 motion; vec4 diffuseAlbedo; vec4 specularAlbedo;
                      vec4 normalRoughness; vec4 sunLight; float transparency; };
SceneOutputs shadeClusterPixel(ClusterPixel p);
// voxel_shading.glsl
struct VoxelQuad { vec3 relative; uvec4 voxel; uvec4 info; float edge; vec2 clipCenter; vec2 halfSize; };
VoxelQuad loadVoxelQuad(SceneBuffer scene, uint list, uint index);  // index within the phase list
SceneOutputs shadeVoxelPixel(vec3 relative, uvec4 voxel, uvec4 info);
```
- `geometricNormal` is unnormalized and unflipped. The forward caller passes `cross(dFdx(rel), dFdy(rel))`; the resolve passes the triangle's edge cross product.
- `terrainSun` is −1 for per-fragment, else 0 or 1, as today.
- The specular-AA normal gradient must stay `dFdx(s.normal)` (the shaded normal, normal map included) on the forward path, or stage 1 is not bit-equal. So `shadeClusterPixel` computes it under `#ifdef CLUSTER_SHADING_SCREEN_DERIVATIVES`, which `mesh.frag` defines. Without the define it uses `p.normalDx/normalDy`, the vertex normal's analytic gradient (the spec's accepted risk: the normal map's share is dropped in the resolve).
- `inTerrainSun` is read only for `ALPHA_MASK` today; `shadeClusterPixel` takes it for every list. Opaque lists get −1 from the cull (`terrainSunCode` returns per-fragment for buckets 0-1), so the result is unchanged.

- [ ] **Step 1:** Move the code; `mesh.mesh`, `mesh.frag`, `voxel_instance.mesh` and `voxel_instance.frag` become thin callers. TERRAIN, DEBUG_ID, DEPTH_ONLY and ALPHA_BLEND paths stay in `mesh.frag`; `writeMeshDebugId` and the interlock section stay there too.
- [ ] **Step 2: Build all shader variants and run ctest**
  Run: `cmake --build build-release 2>&1 | grep -iE 'error' ; ctest --test-dir build-release`
  Expected: no errors; all tests pass.
- [ ] **Step 3: Parity, same views as Task 1 Step 4, plus the overview (`700 1450 -400 -1.55 -0.5`) for the voxels**
  Expected: `--placement off` views (start, edge, close leaf, overview) exactly equal (0.0000) to T1's (c03b527) captures, since this task is a pure refactor. Views with `--trees`: T2 against c03b527 within floor + 0.02/255, where the floor is c03b527 twice; report both.
- [ ] **Step 4: Debug capture**
  Run: `cmake --build build && flock /tmp/meridian-gpu.lock ./build/game/game --trees --camera 600 1205.6 -400 -1.55 0.02 --capture .superpowers/vb/s1-debug.png --frames 30`
  Expected: `0 validation errors`.
- [ ] **Step 5: Commit** (`Shared cluster and voxel shading functions for the visibility resolve`). Stage 1 is then reviewable alone (spec: "Mergeable on its own"; the director decides whether to merge it).

---

## Stage 2: the id target, cluster visibility pipelines, resolve for triangles

### Task 3: Gate tooling (guide dumps, sparkle count, buffer diff)

**Files:**
- Modify: `engine/include/meridian/renderer.h` (`CaptureSource`), `engine/src/render/renderer_frame.cpp` (capture readback), `game/src/main.cpp` (flag), `docs/reference/cli.md`.
- Create: `tools/pfm-diff.py`, `tools/white-dots.py`.

**Interfaces:**
- Produces: `enum class CaptureSource { Display, DiffuseAlbedo, Motion, SpecularAlbedo, NormalRoughness, SunLight };` and the flag `--capture-guides PREFIX` (with `--capture`), which saves `PREFIX-{motion,diffuse,specular,normal,sun}.pfm` (render-sized; specular and normal only when they exist, i.e. under `--rr on`; warn otherwise).
- `python3 -I tools/pfm-diff.py A.pfm B.pfm` prints `mean <per-channel mean |a-b|> max <per-channel max>` and exits 1 on size mismatch or NaN.
- `python3 -I tools/white-dots.py A.png [X0 Y0 X1 Y1]` prints `white-dots <count per 1e4 px>`, counting pixels with OKLab L > 0.85 (decisions.md "Foliage specular AA", m2/s1 figures).

- [ ] **Step 1:** Test `pfm-diff.py` on a synthetic pair: write two 4x4 PFMs from a short Python snippet in the step, with one pixel differing by 0.5. Expected: `max 0.5`, mean 0.5/16 on that channel.
- [ ] **Step 2:** Implement the capture sources by mirroring `DiffuseAlbedo`'s readback path (`enableAlbedoCapture` → generalize to `enableGuideCapture()`, which keeps the diffuse target).
- [ ] **Step 3: Verify**
  Run: `flock /tmp/meridian-gpu.lock ./build-release/game/game --trees --rr on --camera 600 1205.6 -400 -1.55 0.02 --capture .superpowers/vb/t3.png --capture-guides .superpowers/vb/t3`
  Expected: five PFMs. `pfm-diff.py` of a repeat run gives mean < 1e-5 per channel.
- [ ] **Step 4: Commit** with the cli.md lines.

### Task 4: Id packing and barycentrics, CPU reference + GLSL mirror

**Files:**
- Create: `engine/include/meridian/visibility.h`, `engine/src/visibility.cpp` (CPU reference), `engine/shaders/visibility.glsl` (mirror), `tests/visibility_test.cpp`.
- Modify: `engine/CMakeLists.txt` (source), `tests/CMakeLists.txt` (add `visibility_test.cpp` to `add_executable(tests ...)`), `engine/src/render/gpu_types.h` (static_asserts).

**Interfaces (Produces):**
```cpp
namespace meridian {
enum class VisibilityKind : uint32_t { None = 0, ClusterTriangle = 1, Voxel = 2 };
constexpr uint32_t kVisibilityTerrainSunPerFragment = 3;  // the 2-bit form of 255
struct VisibilityId { uint32_t x, y; };
VisibilityId packClusterTriangle(uint32_t instance, uint32_t cluster, uint32_t triangle, uint32_t terrainSunCode /*0,1,255*/);
VisibilityId packVoxel(uint32_t phase, uint32_t index);
VisibilityKind visibilityKind(VisibilityId id);
// Unpacked fields: instance, cluster, triangle, terrainSunCode (back to 0, 1 or 255), phase, index.
struct UnpackedVisibility { VisibilityKind kind; uint32_t instance, cluster, triangle, terrainSunCode, phase, index; };
UnpackedVisibility unpackVisibility(VisibilityId id);

struct Barycentrics { Vec3 b, dbdx, dbdy; bool frontFacing; float outside; };
// Perspective-correct barycentrics of pixel centre `pixel` (framebuffer
// coordinates, y down) in the triangle with clip positions c0..c2 at
// render size `size`; derivatives per pixel step in x and y; `outside` =
// how far the most negative weight is below 0 (0 inside). Facing follows
// Vulkan's signed-area rule for VK_FRONT_FACE_COUNTER_CLOCKWISE in
// framebuffer space.
Barycentrics barycentricsAt(const float c0[4], const float c1[4], const float c2[4], float pixelX, float pixelY, float width, float height);
}
```
GLSL mirrors with the same names in `visibility.glsl` (`uvec2` ids, `vec4` clip positions). The formulation: Schied and Dachsbacher 2015 / The Forge (spec reference), from the three clip positions, with the perspective-correct weights `(b_i / w_i) / sum(b_j / w_j)`.

In gpu_types.h, next to `kMaxVoxelQuads`:
```cpp
static_assert(kMaxInstanceId < (1u << 24));
static_assert(kMaxClusterIndex <= (1u << 26));
static_assert(kMaxVoxelQuads <= (1u << 26));
static_assert(kMaxClusterTriangles <= (1u << 7));  // move kMaxClusterTriangles from cluster.cpp:25 to cluster.h
```

- [ ] **Step 1: Write the failing tests** in `tests/visibility_test.cpp`:
  - `packRoundTrip`: for instance in {0, kMaxInstanceId}, cluster in {0, (1<<26)-1}, triangle in {0, 127}, sun in {0, 1, 255}, `unpackVisibility(packClusterTriangle(...))` returns the inputs and kind ClusterTriangle; `packVoxel(1, kMaxVoxelQuads - 1)` round-trips with kind Voxel.
  - `clearedIsNone`: `visibilityKind({0, 0}) == None`.
  - `barycentricsAtVertices`: a triangle with w = 1, 2, 4; at each vertex's pixel the weights are the unit vector (1e-5).
  - `barycentricDerivativesMatchFiniteDifference`: random triangles (fixed seed, 1000), `dbdx` within 1e-3 relative of `(b(x+0.5) - b(x-0.5))`.
  - `facingMatchesWindingUnderMirror`: a CCW triangle in framebuffer space is front; the same with x mirrored is back.
  - `outsideMeasuresDistance`: a pixel one pixel beyond an edge gives `outside > 0`; the centroid gives 0.
- [ ] **Step 2: Run them and confirm they fail.** Run: `cmake --build build && ./build/tests/tests --test-case='visibility*'` (doctest; name the cases `visibility: packRoundTrip` etc.). Expected: FAIL to build, undefined symbols.
- [ ] **Step 3:** Implement the C++ and the GLSL mirror, with the static_asserts.
- [ ] **Step 4: Run the tests and confirm they pass.** Expected: PASS.
- [ ] **Step 5: Commit** (`Visibility id layout and barycentrics: CPU reference, GLSL mirror, bit budget asserts`)

### Task 5: Id target and visibility pipelines for the cluster lists

**Files:**
- Modify: `engine/src/render/gpu_types.h` (`kVisibilityIdFormat = VK_FORMAT_R32G32_UINT`), `renderer_impl.h` (`Image visibilityId; enum class ScenePipelineSet { Forward, Visibility, Debug }; createScenePipelines(ScenePipelineSet)`; `VkPipeline visibilityPipeline, visibilityMaskedPipeline;`), `renderer_setup.cpp:998-1066`, `renderer_debug.cpp:125-241` (debug toggle returns to the active set), `renderer_frame.cpp:1205-1440`, `renderer_resources.cpp` (id image with the swapchain-sized targets), `vulkan_util.h/.cpp` (`PipelineDesc`), `engine/include/meridian/renderer.h` (`void setVisibilityBuffer(bool on);`, before the first frame), `game/src/main.cpp` (`--visibility off|on`), `engine/CMakeLists.txt` (variants), `docs/reference/cli.md`.
- Shaders: `mesh.mesh` gains `VISIBILITY` (per-primitive `perprimitiveEXT flat out uvec2 outVisibility`, plus position, uv and vertex alpha for masked); new `visibility.frag` (opaque: writes `outVisibility` at location 7; `VISIBILITY_MASKED`: the alpha test of `mesh_masked_depth.frag`, then the write); `VISIBILITY_TARGET` variants `terrain_vt.frag` (mesh.frag TERRAIN), `voxel_vt.frag`, `voxel_instance_vt.frag`, which add `layout(location = 7) out uvec2 outVisibility = uvec2(0)`.

**Interfaces:**
- Consumes: `packClusterTriangle` (visibility.glsl, Task 4); `loadClusterContext`/`clusterVertex` (Task 2).
- Produces: `PipelineDesc::colorWriteMaskBits` (uint32_t, bit i = attachment i writable; default all), which replaces neither `noColorWrites` nor `alphaBlend`. The Visibility set uses eight formats (7 = `kVisibilityIdFormat`):
  - cluster visibility pipelines: bits {7}.
  - sky: bits {0..6}, so the id keeps the clear value.
  - terrain, far field and (until Task 10) voxel quads: all bits, `*_vt` shaders.
  - blended: seven formats, drawn after the resolve (Task 6).
- Frame order with the Visibility set: sky → masked visibility lists (early) → terrain → opaque visibility lists → voxel quads → far field; late: masked visibility → opaque visibility → voxel quads. No masked depth pre-pass and no depth-EQUAL pass. `visibilityId` is cleared to 0 in the early pass (`loadOp CLEAR`) and loaded late.

- [ ] **Step 1:** Implement. Forward (`--visibility off`) must stay byte-identical: it uses the Forward set, which is today's non-debug set.
- [ ] **Step 2: Off is unchanged.** Capture the start, edge, close leaf and overview with `--placement off --visibility off` and compare them with Task 2's captures. Expected: 0.0000.
- [ ] **Step 3: On draws ids.** Run `--visibility on --capture` at the forest edge. Expected: `0 validation errors` (Debug build, `--frames 30`); clusters are not shaded yet, so their pixels show what lies behind (look at the PNG).
- [ ] **Step 4: Cost of the id pass alone.** The `GPU passes:` line at the forest edge, off vs on, Release. Expected: `scene early` + `scene late` drop by at least stage 0's "clusters stripped" saving minus 1 ms (8.2 − 1 = 7.2 ms). Write the raw lines into the ledger (Task 8).
- [ ] **Step 5: VRAM.** The `Loaded: resident` line, off vs on. Expected: +28 MiB (± 1).
- [ ] **Step 6: Debug toggles.** `--visibility on --capture-views` with a list `a <edge>; b <edge> --debug-view lod; c <edge>`. Expected: 0 validation errors (Debug); `c.png` equals `a.png` (mean <= 0.02/255).
- [ ] **Step 7: Commit** (`Visibility id target and cluster visibility pipelines behind --visibility`)

### Task 6: The resolve for cluster triangles

**Files:**
- Create: `engine/shaders/visibility_resolve.frag`.
- Modify: `engine/src/render/renderer_setup.cpp` (resolve layout: sets {atmosphere, material, visibility}, where the new visibility set's binding 0 is a `usampler2D` of `visibilityId`; pipeline `resolvePipeline` with `fullscreen.vert`, the seven scene formats, write bits {0,1,3,4,5,6}, no depth test), `renderer_frame.cpp` (split the late rendering: late lists → end → id to SHADER_READ_ONLY → resolve rendering (attachments 0-6, LOAD) → `endPass("resolve")` → blended rendering → `endPass("blended")`), `gpu_types.h`/`common.glsl` (`SceneData.resolveStats`, a device address of two uint32 counters, "must match"), `game/src/main.cpp` `printStats` (prints `resolve: N pixels, M clamped > 1e-3` over the whole run, from a host-visible buffer read at the end).

**Interfaces:**
- Consumes: `unpackVisibility`, `barycentricsAt` (visibility.glsl); `loadClusterContext`, `clusterVertex` (cluster_vertex.glsl); `shadeClusterPixel` (cluster_shading.glsl).
- The resolve's first lines, before any include, are `#define dFdx(x) VISIBILITY_RESOLVE_FORBIDS_dFdx` and the same for `dFdy`, `fwidth` and `texture`. A shared function that regresses to implicit LOD then fails the build instead of picking a mip from the fullscreen quad.
- Per pixel: kind None → `discard`. ClusterTriangle:
  - Fetch the three vertices with `clusterVertex`, using the triangle's indices from `scene.clusterTriangles`.
  - Run `barycentricsAt` on their `clip`, and clamp weights to [0, 1] renormalized. Count clamping above 1e-3 into `resolveStats`.
  - Interpolate `relative`, `normal`, `tangent`, `uv`, `alpha`, `height`, `crownVisibility`, `currentClip`, `previousClip`, and the gradients (`relativeDx` = Σ dbdx_i · relative_i, likewise uv and normal).
  - `geometricNormal` = cross(rel1 − rel0, rel2 − rel0); `frontFacing` from `barycentricsAt`; the rest of `ClusterPixel` from the cluster context; `terrainSun` from the id.
  - Then `shadeClusterPixel`, writing outputs 0, 1, 3, 4, 5, 6.

  Crown visibility: interpolated from the three per-vertex values. That is bit-closer to forward than evaluating per pixel, and the spec's per-pixel evaluation is a later option.

- [ ] **Step 1:** Implement.
- [ ] **Step 2: Debug capture with `--visibility on`.** Expected: 0 validation errors; `resolve: ... clamped` printed.
- [ ] **Step 3: Quick look.** `--visibility on` vs off at the forest edge and the close leaf view with `--trees` (`image-gates.py diff`), and off twice for the floor. Expected: mean below floor + 0.3/255. Above that, stop and inspect before Task 8.
- [ ] **Step 4: Commit** (`Visibility resolve: cluster triangles shaded once per pixel`)

### Task 7: Stage 2 sweep for the late pass and the far field

**Files:** whatever the grep finds.

- [ ] **Step 1:** Run `grep -nE 'colorAttachmentCount|sceneAttachments|maskedDepthPipeline|maskedMeshPipeline|depthEqualNoWrite|debugPipelines' engine/src/render/*.cpp engine/src/render/*.h`.
  Expected: every hit is either in the Forward/Debug set path or knows about the Visibility set. List each hit and its verdict in the commit message.
- [ ] **Step 2:** Run `--visibility on` at the overview (far cells drawn: `far cells N` > 0 in the capture line) and diff against off. Expected: below floor + 0.3/255 (floor: off twice); no pixel of far cell shows a cluster's shading (inspect at 2x crop).
- [ ] **Step 3: Commit** fixes, if any.

### Task 8: Stage 2 gates (ledger in the research file)

**Files:** Modify `docs/superpowers/research/2026-11-27-vb-stage0.md` → add a "Stage 2 gates" section with every command and raw line.

All runs are Release at 2560x1440, `--trees`, `--visibility off` vs `on`, on the same commit. Write the views file once: start `1119 1355.9 -431 -1.55 -0.1`, edge `600 1205.6 -400 -1.55 0.02`, overview `700 1450 -400 -1.55 -0.5`, s1 `218 1408 1298 -0.785 -0.57`, meadow `1119 1355.9 -431 -1.55 -0.10 --frames 120`, close leaf `1944.3 953.4 -1923.9 -1.2 -0.9`.

- [ ] **Step 1: Look, still.** `flock /tmp/meridian-gpu.lock ./build-release/game/game --trees --visibility X --capture-views views.txt out-X`, then `python3 -I tools/image-gates.py diff out-off/V.png out-on/V.png` per view. Run the off list twice for the floor and repeat the list with `--placement off` (exact or explained). Expected: on minus off below floor + 0.3/255 at every placement view, both figures reported; otherwise `tools/capture-sheet.py` an A/B sheet for the user.
- [ ] **Step 2: Look, motion.** `tools/motion-gates.sh build-release out-mg-X --visibility X` and `tools/motion-clarity.sh build-release out-mc-X --visibility X`. Expected: every row of on at or below off (shimmer, smear, jitter).
- [ ] **Step 3: Guides.** At the edge and s1, `--rr on --capture-guides out/V-X`, then `tools/pfm-diff.py` per buffer. Report mean and max for motion, diffuse, specular, normal and sun.
- [ ] **Step 4: Sparkle.** `tools/white-dots.py` on s1 and edge, off vs on. Expected: on ≤ off + 0.5 per 1e4 px (normal-map specular AA dropped in the resolve, spec risk).
- [ ] **Step 5: Cost.** The `GPU passes:` lines off vs on at the six views, two runs each (spread first). Expected: no view loses more than 0.3 ms. The edge gains at least 6.1 ms and the overview at least 4.0 ms, the latter only after Task 10 (its saving is voxels), so stage 2 reports the overview without gating it.
- [ ] **Step 6: Counters and VRAM.** Every run: `0 dropped`; the cascade need lines unchanged off vs on; `Loaded: resident` +28 MiB.
- [ ] **Step 7: Clamp counter.** `resolve: ... clamped` share below 1e-4 at every view.
- [ ] **Step 8: Debug.** One short Debug capture `--visibility on`, 0 validation errors; `--debug-view lod` with `--visibility on` works.
- [ ] **Step 9: Commit** the ledger.

### Task 9: Midpoint independent review (ordered by the director)

Not run by the implementer. The director dispatches an independent reviewer over `a43f627..HEAD` with the spec, this plan and the Task 8 ledger. Fixes come back as tasks in this plan before Task 10.

---

## Stage 3: voxel quads

### Task 10: Voxel quads through the buffer

**Files:**
- Shaders: `voxel_instance.mesh` gains `VISIBILITY` (`loadVoxelQuad` for the position; per-primitive `outVisibility = packVoxel(phase, first + slot)`; no flat varyings); new `voxel_visibility.frag` (writes location 7 only; no discard). `visibility_resolve.frag` gains the Voxel branch: `loadVoxelQuad(scene, phase * 2, index)` → `shadeVoxelPixel(quad.relative, quad.voxel, quad.info)`.
- Modify: `renderer_setup.cpp` (Visibility set: voxel quads use the visibility pipeline, bits {7}; `voxel_instance_vt.frag` is deleted with its CMake line).

**Interfaces:**
- Consumes: `loadVoxelQuad`, `shadeVoxelPixel` (Task 2); `packVoxel` (Task 4).
- Premise check before Step 1: the phase-0 voxel list must still hold this frame's entries when the resolve runs. The late expansion writes phase 1's region (`voxelQuadListStart(2)`); the cascades write theirs before the main view (architecture.md "sun shadows"). Confirm by reading `expandVoxelBricks` and the cascade path, and record the line numbers in the commit message.

- [ ] **Step 1:** Implement.
- [ ] **Step 2: Debug capture at the overview, `--visibility on`.** Expected: 0 validation errors.
- [ ] **Step 3: Quick look at the overview and s1.** Expected: below floor + 0.3/255 (off twice for the floor).
- [ ] **Step 4: Commit** (`Visibility buffer: voxel quads`)

### Task 11: Stage 3 gates

- [ ] **Step 1:** Repeat Task 8 Steps 1-8 in full. Expected: as there, and now the overview gains at least 4.0 ms.
- [ ] **Step 2: Voxel switch seam.**
  - Find the 27 m beech from `--scatter 100` (`1115.47 1366.85 -429.72 1.223 0`, cli.md) with `--debug-view lod`, then step the camera back along −forward until its crown draws blue, as voxels. That is about 150 m (decisions.md "lone beech at 150 m"); record the pose.
  - At that pose capture `--voxel-near 1000` (triangles) and the default (voxels), each with `--visibility off` and `on`, and measure the crown crop with `tools/region-luminance.py` (linear `.pfm` captures).
  - Expected: tri/vox ratio on within 0.01 of off.
- [ ] **Step 3: Commit** the ledger section "Stage 3 gates".

---

## Stage 4: default on, cleanup, docs

### Task 12: Visibility is the path

**Files:** `renderer_setup.cpp`, `renderer_impl.h`, `renderer_frame.cpp`, `renderer.h`, `game/src/main.cpp`, `engine/CMakeLists.txt`.

- [ ] **Step 1:** Remove the Forward set and `setVisibilityBuffer`. `--visibility` is removed (spec: "selects the path until stage 4"), and the help text and cli.md line go with it.
  - The Debug set keeps the forward pipelines for clusters and voxel quads: `mesh_debug.*`, `mesh_masked_debug.frag`, `voxel_instance_debug.*`, plus the masked depth pre-pass in the debug path.
  - Delete shader variants that no pipeline uses any more (`mesh.frag.spv`, `mesh_masked.frag.spv`, `voxel_instance.frag.spv` if unused) and their CMake lines.
  - `mesh_masked_depth.frag` stays for the debug set.
- [ ] **Step 2:** Build, run ctest, then a Debug capture at the edge (0 validation errors) and a `--debug-view lod` capture (0 validation errors).
- [ ] **Step 3: Commit** (`Visibility buffer by default; forward cluster pipelines only in the debug set`)

### Task 13: Downstream-reference sweep

Before the final review. Each grep's hits are listed with a verdict (fixed / still true / debug-set only) in the commit message.

- [ ] **Step 1: Seven attachments.**
  `grep -rnE 'seven attachments|7 attachments|attachments 0 to 6|debugId \? 8 : 7|size\(sceneAttachments\) - 1|attachment 7' engine docs CLAUDE.md`
- [ ] **Step 2: Masked depth pre-pass and EQUAL.**
  `grep -rnE 'pre-?pass|depth EQUAL|depthEqualNoWrite|maskedDepthPipeline|mesh_masked_depth|maskedMeshPipeline' engine docs CLAUDE.md`
- [ ] **Step 3: Derivatives and implicit LOD in shared GLSL.**
  `grep -nE 'dFd|fwidth|\btexture\(' engine/shaders/*.glsl`. Expected: no hit. Also `grep -rn 'gl_FrontFacing' engine/shaders/*.glsl`: no hit.
- [ ] **Step 4: Frame order and pass names.**
  `grep -rnE 'scene late|blended|endPass\("' docs/reference/*.md tools/*.py tools/*.sh`. `tools/bench-route.py` and the perf overlay must know the `resolve` pass. Also check `game/src/perf_overlay.cpp` for a fixed pass list.
- [ ] **Step 5: The id and debug id.**
  `grep -rnE 'debug id|kDebugIdFormat|maxColorAttachments' docs engine`. architecture.md's "all eight ... are taken" is now true of both sets: say so.
- [ ] **Step 6: Commit** fixes.

### Task 14: Docs

**Files:** `docs/reference/architecture.md` (frame order: visibility lists, resolve pass, late split; "Scene attachments": attachment 7 is the id in the main set, debug id in the debug set; the id layout from this plan's correction 1), `docs/reference/cli.md` (`resolve: N pixels, M clamped` line; `--capture-guides`; pass list gains `resolve`), `docs/decisions.md` (new entry "Visibility buffer (2026-11-27)": technique, sources, the id layout and why 2 bits hold the sun code, the measured gains from Tasks 8 and 11), `docs/backlog.md` (G10, "Visibility buffer", "Visibility buffer before forward GTAO": update or remove), `CLAUDE.md` (only if a convention changed: the "must match" list gains `SceneData.resolveStats` only if it is a struct), `docs/superpowers/resume.md`, and the spec status → done after merge.

- [ ] **Step 1:** Write the docs.
- [ ] **Step 2: Commit.**

### Task 15: Final gates

- [ ] **Step 1:** Task 8 Steps 1-8 and Task 11 Step 2, now default (no flag) against main a43f627's build at the same views. Then the route: `flock /tmp/meridian-gpu.lock ./build-release/game/game --trees --benchmark 60 bench.csv --route tools/routes/parity.route` and `python3 -I tools/bench-route.py bench.csv tools/routes/parity.route` against main's. Expected: no segment worse than main by more than 0.3 ms average.
- [ ] **Step 2:** VRAM at the showcase views: resident main + 28 MiB, peak not above main's.
- [ ] **Step 3: Commit** the ledger section "Final gates".

### Task 16: Independent review and fix check before merge (ordered by the director)

Not run by the implementer. The director orders the independent review of the whole branch against the spec, this plan and the ledgers. A fix check follows any fixes. Only then does the director make the merge call.
