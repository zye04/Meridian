# Virtualized geometry slice 1 (cluster LOD raster) Implementation Plan

**Status:** slice 1 done on main (6af0c51..f1ccccd); slice 5's hierarchical traversal done in M2a Task 7 (`docs/superpowers/plans/2026-11-19-forests.md`); slices 2-4 and slice 5's compression and streaming are deferred, see `docs/backlog.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render every opaque/masked mesh as Nanite-style clusters with continuous LOD, cluster culling and mesh-shader raster.

**Architecture:** meshoptimizer `clusterlod.h` builds a cluster DAG per primitive at load (cached on disk next to the glTF). Cluster tables live in bump-allocated GPU pools. The instance cull emits cluster work items; a new cluster cull applies the LOD cut rule, frustum and Hi-Z tests (Nanite two-phase: previous pyramid, then retest), and appends `(instance, cluster)` to 4 draw lists drawn with `vkCmdDrawMeshTasksIndirectCountEXT`.

**Tech Stack:** C++20, Vulkan 1.3 + VK_EXT_mesh_shader, GLSL (glslc), meshoptimizer v1.0, doctest.

**Spec:** `docs/superpowers/specs/2026-11-19-virtualized-geometry-design.md`

## Global Constraints

- Clusters: max 128 triangles, 128 vertices (`clodDefaultConfig(128)`).
- LOD threshold default 1 pixel; `--lod-error 0` = full detail.
- meshoptimizer pinned `GIT_TAG v1.0`, compiled `-w -O2 -fno-sanitize=all`.
- Blended meshes keep the CPU-sorted indexed path; BLAS stays full-res from original indices.
- Commits: one feature each, no AI attribution trailer; `docs/decisions.md` and `CLAUDE.md` updated in the same commit as the change they describe.
- "Must match" structs: change `gpu_types.h` and GLSL together, static_assert sizes.

## Review Focus

- Meshes smaller than one cluster or with < 4 triangles (single cluster, `refined == -1`, terminal group error FLT_MAX): must still draw. Test in Task 1.
- Non-uniformly scaled instances: error and radius scale by max axis scale (conservative, never coarser than allowed). Covered by the shared `lodVisible` CPU/GLSL formula test in Task 1.
- First frame and after resize, the previous pyramid is invalid: phase 0 must not occlusion-test (no black frame). Verified by Task 4's capture with `--frames 1`.
- Draw-list overflow (more visible clusters than capacity): drop, count, never write out of bounds. Task 3 clamps and reports.
- Stale cache after the source glTF changes: rebuilt, not trusted. Test in Task 2.

---

### Task 1: Cluster DAG build

**Files:**
- Modify: `engine/CMakeLists.txt` (FetchContent meshoptimizer v1.0, `-w -O2 -fno-sanitize=all`, link PRIVATE; include `${meshoptimizer_SOURCE_DIR}/demo` for `clusterlod.h`)
- Create: `engine/include/meridian/cluster.h`, `engine/src/cluster.cpp`
- Modify: `engine/include/meridian/mesh.h` (`MeshData` gains `ClusterLod clusters`)
- Test: `tests/cluster_test.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Produces:
  ```cpp
  struct LodBounds { Vec3 center; float radius = 0; float error = 0; };
  struct Cluster {
    Vec3 boundsCenter; float boundsRadius;   // culling sphere
    LodBounds self;      // own group's simplified bounds (error FLT_MAX if terminal)
    LodBounds refined;   // refined group's bounds; error -1 when original geometry
    uint32_t firstVertex, vertexCount;       // into ClusterLod::vertices
    uint32_t firstTriangle, triangleCount;   // into ClusterLod::triangles (3 bytes each)
  };
  struct ClusterLod {
    std::vector<Cluster> clusters;
    std::vector<uint32_t> vertices;   // mesh-local vertex indices
    std::vector<uint8_t> triangles;   // cluster-local, 3 per triangle
  };
  ClusterLod buildClusterLod(const MeshData &mesh);
  // Projected error in pixels; same formula as cluster_cull.comp.
  float projectedErrorPixels(const LodBounds &b, const Vec3 &camera, float scale,
                             float projY, float nearPlane, float screenHeight);
  // Nanite cut: self over threshold, refined at or under (or original).
  bool lodVisible(const Cluster &c, const Vec3 &camera, float scale, float projY,
                  float nearPlane, float screenHeight, float thresholdPixels);
  ```
  Clusters are built with positions only for simplification weights plus normals/UV as attributes (weights 0.5 normal, 0.1... builder's call), `attribute_protect_mask` covering UV seams.

- [ ] **Step 1: Write failing tests** in `tests/cluster_test.cpp`: a subdivided plane (~20k triangles) and a 2-triangle quad.
  - `TEST_CASE("single small mesh is one original cluster")`: quad -> 1 cluster, `refined.error < 0`, `self.error == FLT_MAX`, `lodVisible` true at any threshold.
  - `TEST_CASE("LOD bounds are monotonic")`: for every cluster with `refined.error >= 0`: `refined.error <= self.error`.
  - `TEST_CASE("selected cut covers the surface once")`: at thresholds {0, 1, 8, 64} px with camera at (0, 5, 20), the sum of areas of triangles in `lodVisible` clusters is within 5% of the plane's area; threshold 0 selects exactly the original triangle count.
  - `TEST_CASE("projected error scales with distance and scale")`: doubling distance (beyond radius) halves the error; scale 2 doubles it.
- [ ] **Step 2:** `cmake --build build && ./build/tests/tests -tc="*cluster*"` -> fails to compile (no header).
- [ ] **Step 3:** Implement with `clodBuild(clodDefaultConfig(128), ...)` and `clodLocalIndices`; the output callback appends clusters and returns the group index; after the build, fill `refined` from the stored group bounds.
- [ ] **Step 4:** tests pass; full `ctest --test-dir build` passes.
- [ ] **Step 5:** Commit "Build Nanite-style cluster LOD hierarchies with meshoptimizer".

### Task 2: On-disk cluster cache and loader

**Files:**
- Modify: `engine/include/meridian/cluster.h`, `engine/src/cluster.cpp`, `engine/src/model.cpp`
- Test: `tests/cluster_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // One file per glTF: every primitive's ClusterLod in mesh/primitive order.
  void writeClusterCache(const std::filesystem::path &file, uint64_t sourceKey,
                         const std::vector<ClusterLod> &lods);
  std::optional<std::vector<ClusterLod>> readClusterCache(
      const std::filesystem::path &file, uint64_t sourceKey);
  ```
  `sourceKey` = hash of format version, glTF file size and mtime. Header: magic `"MCLD"`, version, key, primitive count, then per primitive the three array sizes and raw arrays. Any mismatch or short read -> `nullopt`.
- `loadGltf`: loads all primitives, reads `<stem>.clusters` next to the glTF, else builds each `ClusterLod` (print "Building clusters for <file>") and writes the cache. Also checks per primitive that every cluster vertex index is < vertex count; otherwise rebuild.

- [ ] **Step 1: Failing tests:** `"cluster cache round-trips"` (write, read same key -> identical arrays); `"cluster cache rejects another key"` (read with key+1 -> nullopt); `"truncated cache is rejected"` (file cut in half -> nullopt).
- [ ] **Step 2:** run, fail. **Step 3:** implement. **Step 4:** pass; `./build/game/game --capture /tmp/x.png` loads (cache files appear in `assets/*/`), second run is faster and prints no "Building".
- [ ] **Step 5:** Commit "Cache cluster hierarchies next to the glTF".

### Task 3: GPU cluster tables, cull and mesh-shader raster (no cluster occlusion yet)

**Files:**
- Modify: `engine/src/render/gpu_types.h` (`ClusterInfo` must-match, 64 bytes: culling sphere, self LOD sphere+error, refined error, refined center... builder packs; `MeshInfo` + `firstCluster`, `clusterCount`; `SceneData` + `clusters`, `clusterVertices`, `clusterTriangles` addresses, `lodErrorPixels`, `screenHeight`, `projectionY`); `engine/shaders/common.glsl` (same)
- Modify: `renderer_setup.cpp` (enable `meshShader` feature + `VK_EXT_mesh_shader`; pools; mesh pipelines: `mesh.mesh` + `mesh.frag`/`mesh_masked.frag`; cluster cull pipeline; work-item and draw-list buffers), `renderer_resources.cpp` (`uploadMesh` copies cluster arrays into pools, rebases `firstVertex`/`firstTriangle`), `renderer_frame.cpp`, `renderer_impl.h`, `vulkan_util.*` (mesh pipeline builder if needed), `engine/CMakeLists.txt` (`add_shader` for `mesh.mesh`, `cluster_cull.comp`, `--target-spv=spv1.4` for mesh shaders if glslc requires)
- Create: `engine/shaders/mesh.mesh`, `engine/shaders/cluster_cull.comp`
- Modify: `engine/shaders/cull.comp` (emit work items `(instance, firstCluster, count<=64)` + indirect dispatch args instead of draw commands)
- Modify: `engine/include/meridian/renderer.h` (`setLodErrorPixels(float)`, `drawnClusterCount()`, `drawnTriangleCount()`), `game/src/main.cpp` (`--lod-error PX`, print clusters/triangles with instances)

**Interfaces:**
- Draw list entry: `uvec2(instanceIndex, clusterIndex)`; 4 lists x `kMaxDrawnClusters` (1M each) + counts; indirect args `(count, 1, 1)` written by a 1-thread finalize step or computed in shader via count buffer as `vkCmdDrawMeshTasksIndirectCountEXT` with one command per list whose `groupCountX` = list count (cluster cull atomically increments `commands[list].groupCountX`; reset by `vkCmdFillBuffer` each phase to `(0,1,1)`). Overflowing entries are dropped and counted.
- `mesh.mesh`: `local_size_x = 32`, outputs ≤128 vertices, ≤128 primitives, the same outputs as `mesh.vert` (locations 0-6), reads `drawList[gl_DrawID-free: per-pipeline push constant list address][gl_WorkGroupID.x]`.
- Phase 0/1 for clusters this task: phase 0 draws clusters of instances visible last frame (frustum + LOD); phase 1 draws clusters of newly visible instances. No cluster occlusion yet.

- [ ] **Step 1:** Build passes with shaders compiled; `./build/game/game --capture /tmp/default.png` -> 0 validation errors.
- [ ] **Step 2:** `--lod-error 0` capture at default view vs main's capture (build main in `/tmp` worktree beforehand): mean diff within noise (`magick compare -metric MAE`), triangles drawn == source triangle count of visible instances.
- [ ] **Step 3:** `--lod-error 1` at the high overview `0 300 700 0 -0.35`: triangles drawn well below source; zoom-crop inspection: no cracks.
- [ ] **Step 4:** Commit "Draw meshes as LOD-selected clusters with mesh shaders".

### Task 4: Cluster occlusion culling (Nanite two-phase)

**Files:**
- Modify: `cluster_cull.comp`, `renderer_frame.cpp`, `gpu_types.h`/`common.glsl` (`previousView`), `renderer_impl.h` (`pyramidValid` reset on resize)

**Interfaces:**
- Phase 0: clusters passing LOD + frustum are tested against the existing pyramid (last frame's) with `previousView`; occluded -> appended to a retest list `(instance, cluster)`; skipped when `!pyramidValid`.
- Phase 1: one dispatch over retest list + work items of newly visible instances, tested against the new pyramid.
- Shared occlusion function moves to `cull.glsl` used by `cull.comp` and `cluster_cull.comp`.

- [ ] **Step 1:** `--scatter 3000` captures, occlusion on vs `--no-occlusion`: mean diff within noise at start and open views, drawn clusters lower with occlusion; `--frames 1` capture is not black.
- [ ] **Step 2:** motion capture `--camera-velocity 5 0 0` on vs off: no popping (mean diff within noise).
- [ ] **Step 3:** GPU ms, median of 5, scatter 3000 at start/open/overview, vs main.
- [ ] **Step 4:** Commit "Cull clusters against the depth pyramid in two phases".

### Task 5: Film-weight test asset, measurements, docs

**Files:**
- Modify: `tools/fetch-sample-assets.sh` (one Poly Haven tree with ≥1M triangles), `game/src/main.cpp` (scatter can include it), `docs/decisions.md` (phase 3 slice 1 entry with measurements and sources), `CLAUDE.md` (frame description, layout, new flags, known gaps)

- [ ] **Step 1:** tree loads; capture near and far: triangles drawn far << source; GPU ms recorded.
- [ ] **Step 2:** downstream sweep: `grep -rn "mesh.vert\|DrawIndexedIndirect\|drawCommands\|kDrawBuckets" engine docs CLAUDE.md` and fix stale descriptions.
- [ ] **Step 3:** Commit "Add a film-weight tree and document virtualized geometry".
