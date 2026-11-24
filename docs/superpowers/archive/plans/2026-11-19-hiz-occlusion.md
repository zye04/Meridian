# Hi-Z Occlusion Culling Implementation Plan

**Status:** done on main (a8211f5..5fd09b4).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two-phase Hi-Z occlusion culling on the GPU, proven on a dense Poly Haven scatter at eye height.

**Architecture:** A ground-height query and `--scatter N` give a dense test scene on the real ground. A GPU draw counter and an occlusion toggle give A/B numbers. The depth image becomes storable/sampled and feeds a min-reduced depth pyramid; `cull.comp` runs twice (early: last frame's visible set; late: pyramid test + visibility update) around a split scene pass.

**Tech Stack:** C++20, Vulkan 1.3 (sampler min/max reduction, core 1.2), GLSL compute, doctest.

**Spec:** `docs/superpowers/archive/specs/2026-11-19-hiz-occlusion-design.md`

## Global Constraints

- Commits: one feature per commit, message explains what and why, **no AI attribution trailer**. Push to `main` after each.
- Style: 2-space indent; comments only for a non-obvious why. "Must match" structs change in C++ and GLSL together, with static_assert sizes.
- Every capture run reports `0 validation errors`.
- Keep `CLAUDE.md` (frame order, layout, views) and `docs/decisions.md` current in the commit that changes them.
- GPU ms: median of 5 runs, same window size, noted with the size.
- Image comparisons: MAE against run-to-run noise measured in the same session (~0.001 normalized), not exact pixels.
- Reverse-Z, infinite far plane: depth 1 = near, 0 = far; farthest occluder depth = **min**.

## Review Focus

1. First frame / newly added instances (visibility 0): must still be drawn the same frame by the late pass. (Task 4 Step 5: capture with `--frames 1`.)
2. Window resize: pyramid and its descriptor sets recreated with the swapchain; no stale views. (Task 3 Step 4: resize check.)
3. Camera inside an instance's bounding sphere / sphere crossing the near plane: treated as visible, never culled. (Task 4 Step 2 code + Step 6 eye-height view next to a rock.)
4. Fast camera turn or move: no object missing for a frame. (Task 4 Step 6 motion A/B.)
5. Instances outside the TLAS view still cast shadows (unchanged path): shadowed areas match on/off. (Task 4 Step 6 image comparison.)

---

### Task 1: Ground-level scatter test scene

**Files:**
- Modify: `engine/include/meridian/terrain.h`, `engine/src/terrain.cpp`
- Create: `tests/terrain_test.cpp`; Modify: `tests/CMakeLists.txt`
- Modify: `game/src/main.cpp`, `CLAUDE.md` (views, commands)

**Interfaces:**
- Produces: `float terrainHeight(const TerrainSettings &settings, float x, float z);` — height of the generated mesh surface at (x, z) (bilinear over the grid cell is acceptable; exact at vertices).
- Produces: CLI `--scatter N` (count of placed models; default 0).

- [ ] **Step 1: Failing test** `tests/terrain_test.cpp`: `TEST_CASE("terrainHeight matches mesh vertices")` — generate with default settings, for vertex indices (0,0), (256,300), (512,512), (17,401): `CHECK(terrainHeight(s, v.position.x, v.position.z) == doctest::Approx(v.position.y).epsilon(1e-4))`. Add to `tests/CMakeLists.txt`.
- [ ] **Step 2:** Build; expected: compile error (function missing).
- [ ] **Step 3: Implement** `terrainHeight` by factoring the per-vertex height (noise squared times maxHeight) into a function used by both, plus bilinear interpolation of the four surrounding vertex heights.
- [ ] **Step 4:** `ctest --test-dir build`: PASS.
- [ ] **Step 5: Game.** Default camera start becomes eye height: x 0, z 700, y = `terrainHeight + 1.7`, pitch -0.1. The sample models (helmet, alpha test) sit on the ground a few meters ahead (y = `terrainHeight` at their x,z). `--scatter N`: deterministic RNG (fixed seed), uniform positions within 150 m of the start, random yaw, uniform scale 0.8-1.2, model chosen uniformly from the 15 Poly Haven models (load each once; skip missing ones with the existing "Missing" message), placed at `terrainHeight - 0.05`. Each placement adds one instance per draw item. Error if the instance count would exceed `kMaxInstances` (65536): stop placing and print how many were placed.
- [ ] **Step 6: Verify.** `--scatter 3000 --camera 0 <h+1.7> 700 0 -0.1 --capture`: 0 validation errors; models stand on the ground (look at the capture). Update CLAUDE.md useful views (new default start; scatter view) and remove the "~220 m above" note.
- [ ] **Step 7: Commit**: "Scatter Poly Haven models on the ground and start at eye height".

### Task 2: Drawn-instance counter and occlusion toggle

**Files:**
- Modify: `engine/include/meridian/renderer.h`, `engine/src/render/renderer_impl.h`, `renderer_frame.cpp`, `renderer_setup.cpp`
- Modify: `game/src/main.cpp`

**Interfaces:**
- Produces: `uint32_t Renderer::drawnInstanceCount() const;` — opaque+masked instances drawn in a recent frame (sum of the four bucket counts, copied to a per-frame host-visible buffer after culling, read when that frame's fence is waited).
- Produces: `void Renderer::setOcclusionCulling(bool enabled);` (stored now, used in Task 4; default true).
- Produces: game flag `--no-occlusion`; capture summary line becomes `Rendered N frames, GPU X ms, D drawn instances, E validation errors`.

- [ ] **Step 1:** Implement the counter (vkCmdCopyBuffer of `drawCounts` into the frame's readback buffer after the cull barrier; sum on read). Implement the toggle and flag.
- [ ] **Step 2: Verify.** Default view without scatter: drawn count equals the opaque+masked instances in the frustum (sanity: > 0, ≤ instance count). With `--scatter 3000` at eye height: record drawn count, GPU ms median of 5 (baseline for Task 4).
- [ ] **Step 3: Commit**: "Report drawn instances and add an occlusion culling toggle".

### Task 3: Depth pyramid

**Files:**
- Create: `engine/shaders/depth_pyramid.comp`; Modify: `engine/CMakeLists.txt` (`add_shader`)
- Modify: `renderer_impl.h`, `renderer_setup.cpp` (images, sampler, descriptors, pipeline, swapchain recreate), `renderer_frame.cpp`

**Interfaces:**
- Produces: `Image depthPyramid` (R32F, `pyramidWidth/Height` = previous power of two of the extent, `pyramidLevels` mips, usage STORAGE|SAMPLED), per-mip views, `VkSampler minSampler` (reduction MIN, linear filter, clamp to edge, nearest mip), `buildDepthPyramid(cmd)` that leaves the pyramid in SHADER_READ_ONLY (or GENERAL) for compute reads.
- Depth image usage gains SAMPLED; scene pass depth storeOp STORE.

- [ ] **Step 1:** Enable `features12.samplerFilterMinmax`. Create pyramid, views, min sampler, a descriptor set layout (binding 0 combined sampler source, binding 1 storage image dest) with one set per mip (mip 0 reads the depth image, mip i reads mip i-1 view), compute pipeline with push constant `vec2 destSize`. All recreated in `createSwapchain` / destroyed in `destroySwapchain`.
- [ ] **Step 2:** `depth_pyramid.comp`: one thread per destination texel; `texture(source, (texel + 0.5) / destSize).r` through the min sampler (a 2x2 footprint per texel, so each level is the min of the level above). Record the build after the scene pass with the needed layout transitions/barriers (depth: attachment -> shader read; pyramid: per-mip write -> read barriers).
- [ ] **Step 3:** Build; capture default view and `--scatter 3000` eye height: 0 validation errors, image unchanged vs Task 2 (MAE within noise).
- [ ] **Step 4: Resize check.** Resizing isn't scriptable. Verify by reading that every pyramid object (image, views, descriptor sets) is created in `createSwapchain` and destroyed in `destroySwapchain`; Hyprland's tiling already gives each capture run a different size, so every run exercises creation at a fresh size.
- [ ] **Step 5: Commit**: "Build a min-reduced depth pyramid after the scene pass".

### Task 4: Two-phase occlusion culling

**Files:**
- Modify: `engine/shaders/cull.comp`, `engine/shaders/common.glsl`, `engine/src/render/gpu_types.h` (SceneData, CullConstants)
- Modify: `renderer_impl.h`, `renderer_setup.cpp` (visibility buffer, second draw-count set, cull descriptor for the pyramid), `renderer_frame.cpp`
- Modify: `CLAUDE.md` (Frame section), `docs/decisions.md`

**Interfaces:**
- Consumes: Task 2 toggle/counter, Task 3 pyramid and min sampler.
- Produces: `SceneData` gains `Mat4 view` and `float projectionTerms[4]` = (P00, P11, nearPlane, 0) — must match GLSL. `CullConstants` gains `uint phase` (0 early, 1 late), `uint occlusion` (toggle), visibility buffer address, pyramid size/levels; push size stays ≤ 128 bytes.

- [ ] **Step 1: Buffers.** Visibility buffer (uint per instance, kMaxInstances, zero-initialized; new instances read as 0). Two draw-count sets (early, late) so each pass draws its own lists; drawn-instance counter sums both.
- [ ] **Step 2: cull.comp.**
  - Early phase: `if (visibility[i] == 0 || !inFrustum) return;` append draw.
  - Late phase: `bool visible = inFrustum && (occlusion == 0 || !occluded(center, radius));` `if (visible && visibility[i] == 0) append draw;` `visibility[i] = visible ? 1 : 0;` (instances drawn early but now occluded stop being visible next frame; instances visible but drawn early are not drawn twice).
  - With `occlusion == 0`: early phase draws nothing, late phase draws all in-frustum instances (equivalent to phase 1 behaviour).
  - `occluded()`: view-space center `c = (view * vec4(center,1)).xyz`, depth `d = -c.z`; `if (d - radius < nearPlane) return false;` projected AABB via zeux's `projectSphere` (Mara & McGuire) using P00/P11, converted to pyramid UV; `level = floor(log2(max(width, height) in pyramid texels))`, clamped to the chain; `float occluder = textureLod(depthPyramid, uvCenter, level).r` with the min sampler; `float nearest = nearPlane / (d - radius);` `return nearest < occluder;`.
- [ ] **Step 3: Frame.** cull early -> scene pass 1 (clear; sky + early indirect draws; depth STORE) -> depth pyramid -> cull late -> scene pass 2 (load color/motion/depth; late indirect draws + blended) -> TAA. Barriers: indirect/counts between passes, depth attachment -> sampled -> attachment again.
- [ ] **Step 4: Build.**
- [ ] **Step 5: First frame.** `--scatter 3000 --frames 1 --capture` with occlusion on vs `--no-occlusion`: images match within noise (late pass draws everything the first frame).
- [ ] **Step 6: A/B.** With `--scatter 3000`: eye-height start view; a view behind a hill (pick one where the scatter is hidden: e.g. camera low on the far side of a ridge, find with a few captures); and the start view moving (`--camera-velocity 0 0 -5`) and turning is not scriptable — moving only. For each: on vs off images match within noise; record drawn instances and GPU ms (median of 5) on/off. Expected: occluded view draws far fewer instances and costs less; open view not more than ~0.1 ms worse.
- [ ] **Step 7: Docs.** CLAUDE.md Frame section (new order), decisions.md phase-2 entry (approach, sphere bounds, min pyramid, sources from the spec, measured numbers), "Next" line updated.
- [ ] **Step 8: Commit**: "Two-phase Hi-Z occlusion culling".
