# DAG Voxel Foliage Implementation Plan

Status: in progress, paused 2026-11-24 on branch `g1` (pushed). Done: Tasks
0-5, 6, 6a-6c. Next: Task 9's VRAM/overflow items first (below), then 7, 8, 9.
Execution ledger with every ruling: `2026-11-24-dag-voxel-foliage-ledger.md`
(copy of the git-ignored `.superpowers/sdd/` ledger).

## Resume (paused 2026-11-24)

Findings from the last runs (ledger has details):
- All G1 evidence before 2026-11-24 15:00 ran at 1920x1080 (monitor off,
  Hyprland fallback), not 1440p. Re-measure main and g1 at 1440p.
- At 1440p the forest overview (`--camera 700 1450 -400 -1.55 -0.5
  --trees`) overflows `kMaxVoxelQuads` (8388608, ~0.95M dropped): fix
  before judging the look (Task 7) or GPU time.
- g1 peaks at 16.1-16.7 GB device memory on the 16 GB RTX 4080 (main 14.4
  GB); the far-field splat then takes ~56 s at startup (main 0.36 s). The
  voxel pools' VRAM is the first Task 9 item.
- Repeat-run noise with `--trees`: 0.3/255 at the forest edge, 4.3/255 at
  the overflowing overview. Judge A/Bs against a repeat run, not zero.
- Other lanes' main builds share `assets/` and rewrite the `.clusters`
  caches (main kCacheVersion 7, g1 8): models rebuild when lanes alternate.
- Task 6's Debug far-field check (`build/game/game` at the overview with
  `--trees`, the far-field Debug checks) was started but not finished.
- Task 6c's look verdict (voxel canopy brightness under GI) moves to Task 7.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tree models carry voxel levels inside their cluster LOD DAG, so distant foliage switches per cluster group from triangles to ~1 px voxels that look like the triangles, lit and shadowed the same way, feeding the far field; the per-tree voxel tier and the ray-query bake are removed.

**Architecture:** A vendored fork of meshoptimizer's `clusterlod.h` calls a CPU voxelizer during the DAG build; groups whose voxels beat their triangles' error become voxel clusters (sticky upward, power-of-two lattice per model), cached in `.clusters` v7. On the GPU, voxel clusters ride the existing traversal and cluster cull into their own draw list, are compacted and drawn as camera-facing quads shaded through `lighting.glsl`'s leaf model, cast into the shadow cascades, and the far-field templates come from the 1 m voxel level.

**Tech Stack:** C++20, Vulkan 1.3 mesh shaders, GLSL, meshoptimizer (latest release) + vendored `clusterlod.h` fork, Basis Universal transcoder (CPU RGBA decode), doctest.

**Spec:** `docs/superpowers/specs/2026-11-24-dag-voxel-foliage-design.md`

## Global Constraints

- Acceptance (user): no noticeable voxelization; no noticeable voxel level swaps; proper lighting and shadows. Rigour without a test matrix: unit tests only for new CPU logic, batched side-by-side captures for the user's eye.
- Voxel target = full-detail triangles as the raster draws them (`--voxel-lod off --lod-error 0`); the reference path tracer is information only.
- Performance: P1 no parity route segment slower than the Task 0 baseline; P2 tree passes at forest edge, forest overview, summit >= 20% cheaper in total; P3 Release startup no slower; report VRAM, `.clusters` size, first build time.
- Engine stands alone: opt-in by generic glTF properties only (crown rule: `diffuseTransmission > 0` or alpha MASK, mesh bound over 4 m tall, then every mesh of that model); no asset names.
- No ray queries to build or draw voxels. The TLAS keeps full BLAS for `--shadows rt` and `--reference`.
- `selectClustersBvh` must keep selecting exactly what the GPU traversal + `lodVisible` select, now over mixed triangle/voxel levels.
- Every new GPU struct mirrored in `gpu_types.h` with static_asserts ("must match").
- meshoptimizer at its latest release (verify at fetch time); `clusterlod.h` vendored in `engine/third_party/clusterlod/` with upstream version + commit in its header.
- Commits: no AI attribution trailer; one feature per commit; CLAUDE.md, `docs/reference/*.md`, `docs/decisions.md` current in the same commit as the change.
- Every game run through `flock /tmp/meridian-gpu.lock`; stop only PIDs you started; captures at 2560x1440 with identical `Render WxH` for GPU ms.
- Temp files and capture output under `.superpowers/g1/` in the working checkout, never the shared scratchpad.

## Review Focus

1. Alpha-masked card leaves (MASK, e.g. the Megaplants fallback) must voxelize to their alpha coverage, not to the full card: test in Task 2 ("alpha-masked quad covers its alpha fraction").
2. Placed instances are scaled (placement scale); the voxel clusters' error must scale like triangles' or the switch distance drifts per tree: test in Task 3 ("voxel cluster error scales with instance scale like triangle clusters").
3. A changed texture with unchanged geometry must rebuild the cache (the key hashed geometry only): test in Task 3 ("cluster cache key changes when a voxelized texture changes").
4. A sparse or small model where voxels never win must stay pure triangles and still load: test in Task 3 ("a model whose voxels never beat its triangles has no voxel clusters").
5. The camera close to or inside a voxel's box (grazing views at the switch) must not blow up the projected quad (S = |dx|+|dy|+|dz| >= 1 always; quads clipped by the near plane): test in Task 5 ("voxel quad area is finite and bounded for any view direction").

---

### Task 0: Baseline on main

**Files:**
- Create: `tools/g1-evidence.sh` (captures the evidence views and the fly-toward sequence; args: BUILD OUT [game args])
- Create: `.superpowers/g1/baseline/` (git-ignored output)

**Interfaces:**
- Produces: `tools/g1-evidence.sh BUILD OUT [args]` writing `OUT/{edge,summit,overview,high,horizon}.{png,pfm}`, `OUT/fly_NNN.png`, and `OUT/gpu.txt` (the `GPU passes:` line per view); Tasks 5, 7 and 9 rerun it.

- [ ] **Step 1:** Write `tools/g1-evidence.sh`: Release, `--trees`, views from `docs/reference/cli.md` (forest edge `600 1205.6 -400 -1.55 0.02`, summit ridge `1119 1356 -431 -1.55 -0.02`, forest overview `700 1450 -400 -1.55 -0.5`, high overview `1119 4354 -431 -1.55 -0.35`, far horizon `-359 1408.7 -465 1.57 0 --clouds off`), each also with `--voxel-trees off --lod-error 0` (Task 5 renames the flag to `--voxel-lod off`; the script takes the flag from `VOXEL_OFF_FLAG`, default `--voxel-trees off`); fly-toward: `700 1300 -400 -1.55 -0.1 --camera-velocity -40 -3 0 --capture-sequence 10 --frames 300`.
- [ ] **Step 2:** Run it: `tools/g1-evidence.sh build-release .superpowers/g1/baseline`. Expected: 10 stills, 30 sequence frames, exit 0, no validation errors.
- [ ] **Step 3:** P1: `--benchmark 60 .superpowers/g1/baseline/route.csv --route tools/routes/parity.route --trees`, then `python3 -I tools/bench-route.py .superpowers/g1/baseline/route.csv tools/routes/parity.route > .superpowers/g1/baseline/route.txt`. P3: note Release startup seconds and the `TLAS:`/far-field load lines into `baseline/startup.txt`.
- [ ] **Step 4:** Commit the script (`Add the G1 evidence capture script`).

### Task 1: meshoptimizer latest + vendored clusterlod.h

**Files:**
- Modify: `engine/CMakeLists.txt` (meshoptimizer `GIT_TAG v1.0` -> latest release tag)
- Create: `engine/third_party/clusterlod/clusterlod.h` (upstream copy of that release's `demo/clusterlod.h`, header comment: upstream tag + commit, "modified for voxel levels, see docs/superpowers/specs/2026-11-24-dag-voxel-foliage-design.md")
- Modify: `engine/src/clusterlod.cpp` (include the vendored header), `engine/src/cluster.cpp:23` (`kCacheVersion` 6 -> 7)
- Test: `tests/cluster_test.cpp` (existing)

**Interfaces:**
- Produces: the vendored header compiled through `engine/src/clusterlod.cpp` exactly as today; no behaviour change.

- [ ] **Step 1:** Update the tag, vendor the header unmodified, point the include at it, bump `kCacheVersion` to 7.
- [ ] **Step 2:** `cmake --build build && ./build/tests/tests`. Expected: all pass (the "previous version is rejected" test follows `kCacheVersion`).
- [ ] **Step 3:** Commit (`meshoptimizer <tag>; vendor clusterlod.h for the voxel levels`), CLAUDE.md "Gotchas" cluster-cache line updated to version 7.

### Task 2: CPU voxelizer

**Files:**
- Create: `engine/include/meridian/voxelize.h`, `engine/src/voxelize.cpp`
- Modify: `engine/include/meridian/texture.h`, `engine/src/texture.cpp` (CPU RGBA8 decode)
- Test: `tests/voxelize_test.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Produces:
  - `struct CpuImage { uint32_t width, height; std::vector<uint8_t> rgba; };` and `CpuImage loadKtx2Rgba8(const std::filesystem::path &path, uint32_t maxSize);` (Basis transcode to `cTFRGBA32` at the largest mip <= maxSize)
  - `struct VoxelMaterial { const CpuImage *baseColor; Vec3 baseColorFactor; float alphaCutoff; bool alphaMask; Vec3 transmission; float transmissionFactor; const CpuImage *metalRough; float roughnessFactor; };` (linear factors; null images = constant)
  - `struct VoxelTriangle { Vec3 p[3]; float uv[3][2]; Vec3 n[3]; uint32_t material; };`
  - `struct VoxelPayload { float depth[3]; Vec3 albedo; Vec3 transmission; Vec3 meanNormal; float roughness; };` (depth: optical depth through the voxel along x, y, z; meanNormal unnormalized: its length is the spread, 1 = all aligned)
  - `struct VoxelBrick { int32_t origin[3]; /* lattice voxel coords of its corner */ std::array<VoxelPayload, 64> voxels; uint64_t occupied; };`
  - `struct VoxelGrid { float edge; std::vector<VoxelBrick> bricks; };`
  - `VoxelGrid voxelize(std::span<const VoxelTriangle> triangles, std::span<const VoxelMaterial> materials, float edge);` (lattice anchored at the origin: voxel (i,j,k) spans [i,i+1)*edge; 4x4 rays per voxel face per axis, Loubet-Neyret; optical depth from the blocked fraction f of an axis's rays: `-log(max(1 - f, 1/32))`; payload means weighted by hit count)
  - `VoxelGrid mergeVoxelGrid(const VoxelGrid &fine);` (edge x2; per axis, a coarse voxel keeps the mean transmittance of its 4 two-voxel columns, as decisions.md "Mips keep coverage"; payload means weighted by each child's coverage)
  - `float bruteForceOpticalDepth(std::span<const VoxelTriangle>, std::span<const VoxelMaterial>, Vec3 voxelLo, float edge, int axis, int raysPerSide);` (reference for tests)

- [ ] **Step 1: Write the failing tests** in `tests/voxelize_test.cpp`:
  - "a solid box voxelizes to opaque voxels": a closed 1 m cube of triangles at edge 0.25: every inner voxel `depth[a] >= -log(1/32) - 1e-4` on all axes.
  - "voxel optical depth matches brute-force rays": a random leaf soup (200 quads, seed 1) at edge 0.5: for every occupied voxel and axis, `|depth - bruteForceOpticalDepth(..., 16)| <= 0.15`.
  - "alpha-masked quad covers its alpha fraction": a 1 m quad, MASK, cutoff 0.5, base colour image left half alpha 1, right half 0: blocked fraction along its normal within 0.5 +- 0.07.
  - "albedo and transmission are hit means": two quads in one voxel with albedo (1,0,0) and (0,0,1), equal area: albedo within 0.05 of (0.5,0,0.5); transmission likewise.
  - "merged grid keeps coverage": the leaf soup at 0.25 merged to 0.5: per-axis mean transmittance over the volume within 3% of the fine grid's.
  - "lattice is anchored at the origin": a triangle near (0.3, 0, 0) at edge 0.25 lands in brick origin 0 / voxel x=1.
- [ ] **Step 2:** `cmake --build build && ./build/tests/tests -tc="*voxel*"` -> FAIL (undefined).
- [ ] **Step 3:** Implement `voxelize.cpp` from `git show farm-cpuvox:tools/cpuvox/cpuvox.cpp` (its triangle binning and per-face ray grid), adding UV interpolation at hits and bilinear texture sampling of `CpuImage`; multithread over bricks with `std::for_each(std::execution::par, ...)`; `loadKtx2Rgba8` in texture.cpp.
- [ ] **Step 4:** Tests pass; also run the whole suite.
- [ ] **Step 5:** Commit (`CPU voxelizer: textured payload, origin-anchored lattice, coverage-keeping merge`).

### Task 3: Voxel levels in the DAG build

**Files:**
- Modify: `engine/third_party/clusterlod/clusterlod.h` (hook), `engine/include/meridian/cluster.h`, `engine/src/cluster.cpp`, `engine/include/meridian/mesh.h`, `engine/src/model.cpp`
- Test: `tests/cluster_test.cpp`

**Interfaces:**
- Consumes: Task 2's `voxelize`, `mergeVoxelGrid`, `VoxelMaterial`, `VoxelTriangle`.
- Produces:
  - In the fork: `clodConfig::voxelize` callback `int (*)(void *context, const unsigned int *sourceTriangles, size_t count, float error, clodVoxelOut *out)` called per group before simplifying; it returns 1 when the group became voxels (clodBuild then emits the group's voxel clusters instead of simplified triangles and marks every group built from it as voxel, calling the callback again with the union of their source triangles). The fork tracks each cluster's source (original) triangle indices.
  - `Cluster` gains `uint32_t firstBrick = 0, brickCount = 0;` (brickCount > 0 = voxel cluster, triangle fields 0) and `float voxelEdge = 0;`.
  - `ClusterLod` gains `std::vector<VoxelBrick> bricks;` and per model (in the cache, not per mesh) `ModelVoxelInfo { Vec3 boundsLo, boundsHi; }`.
  - `MeshData` gains `bool voxelize = false;` (set by the model crown rule) and the material pointer used for sampling.
  - `ClusterLod buildClusterLod(const MeshData &mesh, const VoxelSource *voxels = nullptr);` where `struct VoxelSource { std::span<const VoxelMaterial> materials; uint32_t materialOfMesh; };`
  - Voxel error rule: lattice edge e = smallest 2^k >= the group's QEM error (the error before `keepLeafArea`'s move); while the group's voxels need more than 128 bricks per voxel cluster (split into clusters of <= 128 bricks spatially), double e; the group becomes voxels when the final e <= 2 x that QEM error (one lattice step of slack, since e is rounded up to a power of two), otherwise it stays triangles and is offered again at the next level; a voxel group's `self.error = e`.
  - Box axis depths: after all meshes of a model are built, `void bakeModelAxisDepths(std::vector<ClusterLod> &meshes);` writes per voxel the 6 axis optical depths out of the model's voxel box (through every mesh's voxels at the same lattice edge), stored in a parallel `std::vector<std::array<uint8_t,6>> axisDepth` of `ClusterLod` (4-bit steps of `kProtoAxisDepthStep` 0.5 like today, kept as bytes on the CPU).
  - Cache: `writeClusterCache`/`readClusterCache` store bricks and axis depths; `geometryKey` -> `clusterSourceKey(meshes, materials)` hashing geometry, opted-in flags, the voxelized textures' file bytes and `kVoxelizerVersion = 1`.

- [ ] **Step 1: Write the failing tests** (cluster_test.cpp, synthetic tree: 2000 disjoint leaf quads in a 6 m crown + a cylinder trunk, all opted in):
  - "voxel levels appear above the triangles and stay voxels": some clusters have `brickCount > 0`; every cluster whose `refined` group is voxels is voxels.
  - "the selected cut covers the surface once over mixed levels": reuse the existing coverage check (sample points on the original leaves; each covered exactly once) for 20 camera distances 5-2000 m.
  - "selectClustersBvh equals lodVisible over mixed levels": as the existing BVH test, mixed DAG.
  - "voxel cluster error scales with instance scale like triangle clusters" (Review Focus 2): `projectedErrorPixels` of a voxel cluster at scale 2 equals 2x at scale 1 (same as a triangle cluster).
  - "a model whose voxels never beat its triangles has no voxel clusters" (Review Focus 4): a single 1 m solid box opted in: `bricks.empty()`.
  - "cluster cache key changes when a voxelized texture changes" (Review Focus 3): same geometry, one texel changed in the base colour file -> different key.
  - "voxel bricks and axis depths round-trip the cache".
- [ ] **Step 2:** Run -> FAIL.
- [ ] **Step 3:** Implement the fork hook, the callback in `buildClusterLod`, the crown rule in `model.cpp` (a model opts in when any primitive's material has `diffuseTransmission > 0` or MASK and its bound's height > 4 m; then all its primitives set `voxelize`), `bakeModelAxisDepths`, cache v7 fields, `clusterSourceKey`. Keep `keepLeafArea` (Task 7 measures removing it).
- [ ] **Step 4:** Tests pass. Then Release `--trees --placement off` startup once: prints "Building clusters" for the Megascans variants; record first-build seconds and `.clusters` sizes to `.superpowers/g1/build.txt` (P3).
- [ ] **Step 5:** Commit (`Voxel levels inside the cluster DAG`).

### Task 4: GPU data and selection

**Files:**
- Modify: `engine/src/render/gpu_types.h` (`ClusterInfo` + `GpuVoxel`), `engine/shaders/common.glsl` (mirror), `engine/src/render/renderer_resources.cpp` (voxel pool upload, `grownPool`), `engine/shaders/cluster_cull.comp` (voxel clusters -> voxel draw list), `engine/src/render/renderer_frame.cpp` (list reset, counters), `engine/src/render/renderer_impl.h`
- Test: Debug run check

**Interfaces:**
- Consumes: Task 3's `Cluster::firstBrick/brickCount/voxelEdge`, `ClusterLod::bricks/axisDepth`.
- Produces:
  - `ClusterInfo` keeps 64 B-aligned layout; voxel clusters reuse `firstTriangle` as first brick and `triangleCount` as brick count with a flag bit 31 in `vertexCount` (`kVoxelClusterFlag`); `voxelEdge` derivable as `selfError` (static_assert unchanged sizes).
  - `struct GpuVoxel { uint32_t depthSpread; uint32_t albedoRoughness; uint32_t transmission; uint32_t normal; };` 16 B: `depthSpread` = optical depth along x, y, z (8 bits each, step 1/16) + normal spread (8 bits, length of the mean normal); `albedoRoughness` = linear RGB8 + perceptual roughness 8; `transmission` = linear RGB8 + 8 unused; `normal` = octahedral 2 x 16 bit. The 6 box axis depths (4 bits each) live in a parallel `uint32_t` array (`voxelAxisPool`). The builder may repack if a field measures short of precision.
  - Brick record `GpuVoxelBrick { int16_t origin[3]; uint16_t pad; uint64_t occupied; uint32_t firstVoxel; uint32_t edgeBits; }` 24 B (voxels compacted: only occupied ones stored, ordered by bit index).
  - Draw list `kVoxelClusterList` beside the 4 triangle lists; entries as today (instance | sun code << 24, cluster).
  - Debug `voxel check:` line: GPU-selected voxel cluster count vs `selectClustersBvh` at the capture view (0 mismatches) for the first placed tree.

- [ ] **Step 1:** Mirror structs; upload bricks/voxels/axis words into pools; `cluster_cull.comp` routes voxel clusters to `kVoxelClusterList` (same LOD/frustum/Hi-Z tests; the voxel LOD threshold multiplier `kVoxelPixels` = 1.0 pushed as a constant, tuned in Task 7).
- [ ] **Step 2:** Debug build, `./build/game/game --trees --capture .superpowers/g1/t4.png` at the forest overview (old voxel trees still drawing; the new list only counted). Expected: `voxel check: 0 mismatches`, no validation errors, nonzero voxel clusters listed.
- [ ] **Step 3:** Commit (`Voxel clusters on the GPU: pools, selection, draw list`).

### Task 5: Voxel raster and shading; old tier removed

**Files:**
- Create: `engine/shaders/voxel_compact.comp`, `engine/shaders/voxel_cluster.mesh`, `engine/shaders/voxel_cluster.frag`, `engine/shaders/voxel.glsl` (payload decode, quad area rule, coverage test)
- Modify: `engine/shaders/lighting.glsl` (aggregate-leaf entry point), `engine/src/render/renderer_frame.cpp`, `renderer_setup.cpp` (pipelines), `engine/CMakeLists.txt` (`add_shader`), `game/src/main.cpp` (`--voxel-lod on|off` replaces `--voxel-trees`), `engine/include/meridian/renderer.h`
- Delete: `engine/include/meridian/protovoxel.h`, `engine/src/protovoxel.cpp`, `engine/shaders/protovoxel.glsl`, `voxel_bricks.comp`, `voxel_instance.mesh`, `voxel_instance.frag`, `voxel_band.frag`, `tests/protovoxel_test.cpp`; the per-tree switch in `cull.comp`; the prototype-voxel part of `renderer_farfield.cpp` (the far-field templates keep coming from the bake until Task 6)
- Test: `tests/voxel_shape_test.cpp`

**Interfaces:**
- Consumes: Task 4's list, `GpuVoxel`, `GpuVoxelBrick`.
- Produces:
  - `float voxelQuadArea(Vec3 viewDirLocal, float edge)` CPU mirror in `engine/include/meridian/voxel_shape.h` (edge^2 * (|dx|+|dy|+|dz|), the view direction normalized; matches `voxel.glsl`).
  - `float voxelViewOpacity(const float depth[3], Vec3 viewDirLocal)` CPU mirror (optical depth along the view = blend of the axis depths by d^2, chord rule `v / S`; opacity `1 - exp(-depth)`).
  - Lighting: `void shadeLeafAggregate(SurfaceInput s, float normalSpread, vec3 transmission, ...)` in `lighting.glsl`: front diffuse, transmission and GGX weighted by the lit/transmitted fractions of a normal lobe of that spread (for spread -> 1 it equals the leaf triangles' shading with that normal: assert by a CPU mirror test).
  - `--voxel-lod off`: the voxel LOD threshold becomes infinite (triangles everywhere) for the evidence A/B.

- [ ] **Step 1: Write the failing tests** (`tests/voxel_shape_test.cpp`):
  - "voxel quad area is finite and bounded for any view direction" (Review Focus 5): for 10k random unit directions, `edge^2 <= area <= sqrt(3) edge^2 + 1e-5`.
  - "view opacity equals the axis opacity along an axis": `voxelViewOpacity(d, (1,0,0)) == 1 - exp(-d[0])`.
  - "aggregate shading equals leaf shading for an aligned lobe": spread 1 vs the triangle leaf term, 100 random sun/view/normal triples, max abs diff < 1e-4 (CPU mirror of the GLSL function in the test).
- [ ] **Step 2:** Run -> FAIL; implement `voxel_shape.h`; tests pass.
- [ ] **Step 3:** Implement the GPU path: `voxel_compact.comp` (one workgroup per listed voxel cluster: bricks in the frustum, voxels whose box-exit depth toward the camera <= 3.5, compacted 64 per mesh workgroup, indirect dispatch), `voxel_cluster.mesh` (one quad per voxel, area rule, exact depth at the voxel centre's projected plane, motion from instance transform), `voxel_cluster.frag` (coverage: candidate (a) stable hash of (instance, cluster, voxel) vs `voxelViewOpacity`; no discard in the main pipeline: coverage decided per voxel in the mesh shader; shading through `shadeLeafAggregate`, self-shadow from the box axis depths, cascades from where the sun ray leaves the box as `voxel_instance.frag` does today, cloud shadow, GI ambient; writes depth, motion, guides, transparency mask 0). Early and late scene passes draw the list after the triangle lists. Delete the old tier and `--voxel-trees`.
- [ ] **Step 4:** Debug capture at the forest overview and forest edge: no validation errors, `voxel check: 0 mismatches`; Release `tools/g1-evidence.sh build-release .superpowers/g1/t5` with `VOXEL_OFF_FLAG='--voxel-lod off'`. Expected: forests drawn, no holes vs the baseline captures (look), GPU passes listed.
- [ ] **Step 5:** Commit (`Voxel clusters drawn and lit like leaves; per-tree voxel tier removed`); CLAUDE.md + `docs/reference/{cli,architecture}.md` updated (flag, frame order, layout).

### Task 6: Far field from the DAG voxels

**Files:**
- Modify: `engine/src/render/renderer_farfield.cpp`, `engine/include/meridian/farfield.h`, `engine/src/farfield.cpp`, `engine/shaders/farfield_splat.comp`, `engine/shaders/farfield.glsl`, `engine/shaders/voxel.frag`, `engine/shaders/gi_probes.comp` (through `farFieldTransmittanceScatter`), `engine/shaders/farfield_shadow.comp`
- Delete: `engine/shaders/farfield_bake.comp` and its pipeline
- Test: `tests/farfield_test.cpp`

**Interfaces:**
- Consumes: Task 3's per-model voxel levels (the 1 m lattice level, `edge == kFarTemplateVoxel`, merged from finer ones with `mergeVoxelGrid` where a model has no 1 m level).
- Produces: `std::vector<FarTemplate> farTemplatesFromVoxels(const std::vector<ClusterLod> &modelMeshes);` replacing the bake's output; the world-brick voxel word carries RGB albedo (RGB565) and transmission instead of the palette index (`packVoxel` changes; `farFieldTransmittanceCpu` mirror updated).

- [ ] **Step 1: Write the failing tests:** "far templates from voxels keep each prototype's coverage" (template coverage per axis within 3% of the model's 1 m voxel level), "packVoxel round-trips RGB albedo and transmission" (within 1/31 per channel).
- [ ] **Step 2:** Run -> FAIL; implement; pass; the existing far-field tests (splat vs CPU replica, `farFieldTransmittance` vs CPU) updated and passing.
- [ ] **Step 3:** Debug capture at the forest overview with `--trees`: `far shadow map check` and the far-field Debug checks pass; Release startup time into `.superpowers/g1/t6-startup.txt` (P3: expected faster, no bake).
- [ ] **Step 4:** Commit (`Far field from the DAG voxels; ray-query bake removed`); backlog row "Far-field bake needs ray queries" removed.

### Review addendum (2026-11-24): Tasks 6a-6c, before Task 7

From the Fable review (`.superpowers/fable-review/findings.md`: R6-3, R6-2,
R3-1/R6-5, R6-A1). They come before Task 7 so its quality loop doesn't judge
a double-drawn band at the triangle-to-voxel switch, or a voxel canopy that
GI darkens twice.

How the cut behaves today (it bears on every option below). Take a group G
whose triangles go to voxels at lattice edge e. G's error is
gE = max(e, childrenError), which lies in [e, 2e). The voxel roots (lattice e)
have self group P1 with error 2e, tested at 2T, and refined G, tested at T
(`vertexCount = 1`). Lattice 2e has self P2 = 4e and refined P1, both
tested at 2T. T is `lodErrorPixels` and K is the pixels per metre at
distance 1.
- G's triangles draw while gE·K/d > T. Lattice 2e draws from e·K/d <= T.
  Both draw while e·K/d <= T < gE·K/d (R6-2), a band whenever gE > e.
- The lattice-e roots draw only where proj(P1) > 2T and proj(G) <= T. With
  P1 at 2e and gE >= e, those two conditions meet only through sphere slack.
  The finest voxel level is therefore close to never drawn: the triangles
  hand over directly to 2 px lattice-2e voxels. That is the design's 2 px
  intent, but lattice e is cache and VRAM that almost never draws. This is
  a Task 9 question, not a defect.
- The cut covers the surface once exactly when, for every root cluster,
  proj(P1) >= 2·proj(G). That is the R6-2 test. Spheres only help, because
  P1 encloses G.

### Task 6a: CPU reference carries the GPU's voxel thresholds (R6-3)

**Files:**
- Modify: `engine/shaders/common.glsl` (comment: mirrors it), `engine/include/meridian/cluster.h`, `engine/src/cluster.cpp` (`lodVisible`, `selectClustersBvh`), `engine/src/render/renderer_resources.cpp` (upload uses the shared rule)
- Test: `tests/cluster_test.cpp`

**Interfaces:**
- Produces:
  - `inline constexpr float kVoxelPixels = 2.0f;` in `cluster.h`, because the CPU reference needs it outside the renderer (`gpu_types.h` is renderer-private). common.glsl's "must match" then points at cluster.h.
  - `struct LodThresholds { float self, refined; };` and `LodThresholds clusterThresholds(const ClusterLod &lod, uint32_t cluster, float threshold)`. A triangle cluster gets {T, T}. A voxel cluster gets self 2T, and refined T if its refined group is triangles, else 2T. Upload's `vertexCount` flag derives from this same function, replacing the inline `triangleGroup` code at renderer_resources.cpp:441, so the CPU rule and the GPU's input can't drift.
  - `lodVisible(cluster, ..., LodThresholds)`. `selectClustersBvh` seeds levels at `finest <= T·kVoxelPixels` (as cull.comp:44) and prunes nodes at T (as cluster_traverse.comp; conservative), so it still returns exactly what `lodVisible` accepts.

- [ ] **Step 1: Write the failing test.** "every voxel root switches no later than its triangles" (findings R6-2): for every voxel cluster whose refined group is triangles in `cachedCrown()`, and cameras at 20..3000 m along two directions, assert `projectedErrorPixels(c.self) >= kVoxelPixels * projectedErrorPixels(c.refined)`. Check that it fails on `cachedCrown()`. If the synthetic crown never produces gE > e (QEM simplifies it almost exactly), add a case whose children error is not a power of two, for example a crown with `minEdge` chosen to force it. A test that cannot fail does not count.
- [ ] **Step 2:** Implement `kVoxelPixels`, `clusterThresholds`, the new `lodVisible`/`selectClustersBvh` and the upload change. The existing "cut covers once" and "selectClustersBvh equals lodVisible over mixed levels" tests now run on the GPU rule; Step 1's test stays red until Task 6b.
- [ ] **Step 3:** Commit (`CPU LOD reference carries the voxel thresholds`), with the CLAUDE.md `selectClustersBvh` line updated (it now names the voxel thresholds).

### Task 6b: Exact triangle-to-voxel junction (R6-2)

**Options** (acceptance for all: Task 6a's test passes; existing tests pass):

| | Change | Switch distance | Cost | Cut covers once |
|---|---|---|---|---|
| A | `edgeFor` picks a power of two in [childrenError, maxError]. With none in that interval, the group stays triangles one more pass, and G's error = e | Unchanged where gE = e already. Groups with childrenError > e switch one or more passes later, at a coarser edge | More triangle levels for those groups: the thinning the voxels exist to stop. maxError is often < 2·childrenError, so many groups get no power of two for several passes | Yes |
| B | P1's error = max(2e, 2·max gE over its roots' refined groups) instead of `edge * 2` (cluster.cpp:524, roots' first level only). Higher levels keep 2^k·e, which is still >= P1 because gE < 2e | Triangles unchanged (gE). Lattice-2e voxels start at gE_max·K/T instead of e·K/T, at 2e·T/gE_max ∈ (1, 2]·T px | Removes the overlap band's voxels, so it is cheaper than today. Where a pooled cell mixes groups with different gE, lattice-e voxels (<= 1 px) fill [gE, gE_max]·K/T for the smaller-gE groups: a thin band of fine voxels | Yes (P1 >= 2·gE, P1 encloses G) |
| C | Drop the 2x on P1's self test (and so on lattice 2e's refined test) | Triangles unchanged | Lattice 2e is then never drawn: lattice e draws at 0.5-1 px over a 2x distance band, then lattice 4e. That is the sub-pixel voxel cost the 2 px rule was introduced to avoid (brick count grows with the cube) | Yes |

**Recommendation: B.** It is one build-side change. It leaves the triangle switch distances and the Task 3 error rulings as they are, and costs less than today. A risks reintroducing leaf thinning. C brings back the sub-pixel voxel cost.

**Files:**
- Modify: `engine/src/cluster.cpp` (`finish`: the first voxel group's error over root members), `engine/shaders/common.glsl` (rewrite the "cut stays exact" comment to state the rule: P1 >= 2·G), `docs/superpowers/specs/2026-11-24-dag-voxel-foliage-design.md` (the cut-exactness lines 331/364 now name the rule)
- Test: `tests/cluster_test.cpp` (Task 6a's test)

- [ ] **Step 1:** Task 6a's test is red. Implement B and run the tests: green.
- [ ] **Step 2:** The `.clusters` cache rebuilds through `kVoxelizerVersion` + 1, because the payload is unchanged but the group errors are not. Release `--trees` forest edge `600 1205.6 -400 -1.55 0.02` and overview `700 1450 -400 -1.55 -0.5`. Expected: voxel quad count down at the edge, "tree passes" ms no higher, and no visible change outside the former band (mean difference small, concentrated at the switch distance).
- [ ] **Step 3:** Commit (`Exact triangle-to-voxel junction: first voxel level's error covers its triangles' error`), with decisions.md "Voxels inside the cluster DAG" amended.

### Task 6c: Voxel self-canopy occlusion as the triangles get it (R3-1 / R6-5)

**Files:**
- Modify: `engine/shaders/voxel_instance.frag` (`s.occlusion`)

**Interfaces:**
- `s.occlusion = giMode != 0 ? 1.0 : mix(1.0, 0.5 * (up + sides), microShadowStrength)`, the same treatment as the clusters' `crownOcclusion()` in surface.glsl. Under GI the probes already trace the canopy. Without GI the axis-depth term stands in for `crownVisibility`, scaled the same way. The sun self-shadow (`selfDepth`) is unchanged.

- [ ] **Step 1:** Implement.
- [ ] **Step 2:** A/B in Release, 3 captures in one batch: overview `--camera 700 1450 -400 -1.55 -0.5 --trees` before and after; `--gi off` after as the control. Expected: brighter voxel canopy under GI (region luminance on the far forest up, toward the triangle trees near the switch); `--gi off` unchanged except where `microShadowStrength` < 1. Any other visible change: stop.
- [ ] **Step 3:** Commit (`Voxel canopy occlusion only without GI, scaled like the clusters'`), with the progress.md note "darker" in the overview measurements re-checked against the after capture.

### On the Task 4+5 ruling "the Debug voxel check is not built" (R6-A1)

Task 6a removes the ruling's broken premise. The CPU reference then runs the GPU's thresholds, and the upload's `vertexCount` flag comes from the same function. What stays unmirrored is GLSL only: the two threshold lines in cluster_cull.comp, the seed bound in cull.comp, and the prune in cluster_traverse.comp. Recommendation: keep not building the Debug check, and add a line to those three shaders naming `clusterThresholds` as the reference. Cost if wrong: a GLSL-only slip in those lines shows only in captures, which is the current state for every other cull rule.

### Task 7: Quality loop to the acceptance criteria

**Files:**
- Modify: `engine/shaders/voxel.glsl`, `voxel_cluster.mesh/.frag`, `cluster_cull.comp` (`kVoxelPixels`), `engine/src/cluster.cpp` (keepLeafArea decision)
- Evidence: `.superpowers/g1/t7-*/`

**Interfaces:**
- Consumes: Tasks 5-6.
- Produces: the chosen coverage rule, voxel pixel size, normal-spread model; decisions recorded in `docs/decisions.md` "Voxels inside the cluster DAG".

- [ ] **Step 1:** Evidence run per candidate, batched in one session: coverage (a) stable hash vs (b) hash cycled over the jitter phases; `kVoxelPixels` 1 vs 2; `keepLeafArea` on vs off below voxel levels. Per run: `tools/g1-evidence.sh` + `tools/motion-clarity.sh` forest rows. Pick by: forest-crop Laplacian std <= the full-detail triangles' (no speckle), region luminance within 5% far and 3% at the switch, still jitter no worse than triangles, fly-toward frame differences at switch frames no higher than neighbours.
- [ ] **Step 2:** Apply the winners; rerun the evidence once; show the user the side-by-sides (switch distances + far views + fly-toward) and get their eye on criteria 1-2.
- [ ] **Step 3:** Commit (`Voxel look: coverage rule, voxel size, leaf area`) with the decisions entry.

### Task 8: Shadows from voxel clusters

**Files:**
- Modify: `engine/src/render/renderer_shadows.cpp`, `engine/shaders/cluster_cull.comp` (shadowView: voxel clusters to a shadow voxel list), create `engine/shaders/voxel_cluster_shadow.frag` (depth-only; coverage by the same rule as the main pass along the sun), pipelines
- Evidence: forest overview and summit at sun elevations 0.6 and 0.15 (`--sun`), with and without

**Interfaces:**
- Consumes: Task 5's compaction and mesh shader (shadow variant: quads facing the sun, orthographic area rule).
- Produces: voxel clusters cast into cascades 0-3 under `shadowView` with their own counters (as `far-m-shadow2` split them); the coarsest cascade keeps casting far cells.

- [ ] **Step 1:** Implement; Debug capture: no validation errors, `sun shadows:` line max ms reported.
- [ ] **Step 2:** Release evidence: distant trees cast ground shadows at the forest overview (G11); a voxel tree is not darker than its triangle version at the switch (self-shadow counted once). Show the user.
- [ ] **Step 3:** Commit (`Voxel clusters cast into the shadow cascades (G11)`); close the backlog G11 row and issue #11 with the evidence; delete branch `far-m-shadow2` (local + origin) after the user OKs.

### Task 9: Performance pass and closeout

**Files:**
- Modify: whatever the measurements point at (compaction, list sizes, LOD threshold), docs
- Evidence: `.superpowers/g1/final/`

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1:** P1/P2/P3 against Task 0: parity route (`--benchmark 60 ... --route tools/routes/parity.route --trees`, `bench-route.py`), `GPU_RUNS=3` per-pass medians at forest edge, overview, summit; startup; VRAM (`Placed`/pool lines); `.clusters` sizes. Fix until P1 holds and P2 >= 20%.
- [ ] **Step 2:** Debug once with ASan/validation over the evidence views (Debug startup with placement ~95 s).
- [ ] **Step 3:** Docs: decisions.md entry final numbers; CLAUDE.md (cache version, `--voxel-lod`), `docs/reference/{cli,architecture}.md`; backlog: remove G1, "Voxel switch band with opaque leaves", "Crown light optical depth from the prototype voxel grids"; spec Status "implemented"; this plan's Status "done"; resume.md; close issue #1 with the final side-by-sides after the user's OK; delete branch `farm-cpuvox` (origin too).
- [ ] **Step 4:** Commit (`G1 closeout: performance, docs, backlog`), push.
