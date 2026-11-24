# Virtualized geometry (GPU-driven phase 3) — design

**Status:** slice 1 done on main (6af0c51..f1ccccd); slice 5's hierarchical traversal done in M2a Task 7 (`docs/superpowers/archive/plans/2026-11-19-forests.md`); slices 2-4 and slice 5's compression and streaming are deferred, see `docs/backlog.md`.

## Intent

Make film-grade assets usable: multi-million-triangle meshes (Poly Haven
trees at 0.5-7M triangles, scans) rendered at a cost that follows what is on
screen, not what is in the file. Nanite-style: meshes become clusters of up
to 128 triangles arranged in a LOD DAG; the GPU picks, per cluster, the
coarsest version whose error is under a pixel, culls clusters against the
frustum and the Hi-Z pyramid, and rasterizes the survivors.

The user wants "Nanite, modernized and tailored": keep its strengths, fix
its known weaknesses (UE 5.7). Agreed differentiators:

1. **Ray tracing that matches the raster.** Nanite traces coarse fallback
   meshes; we trace the same clusters at the same LOD (cluster acceleration
   structures: `VK_NV_cluster_acceleration_structure` now; DXR shipped the
   cross-vendor equivalent in Oct 2026 with AMD RX 9000 support, so a Vulkan
   EXT is expected and the data model carries over). NVIDIA-only is accepted
   as a start; the user prefers not to stay vendor-exclusive.
2. **Foliage that survives simplification.** Disconnected leaves thin out
   and lose area under Nanite; UE 5.7's voxel foliage is experimental and
   has no alpha mask. We want coverage-preserving simplification and a
   volumetric far representation.
3. **Tiny triangles shaded efficiently** (visibility buffer), required
   either way.
4. **No shimmer/smear** from sub-pixel detail: stable screen-error LOD
   selection, DLSS later.
5. Compression and streaming (later, at 15 km world scale).

## Slices (one brainstorm/design/plan/commit cycle each, in order)

1. **Cluster LOD raster** (this document in detail): cluster DAG (cached),
   GPU instance + cluster culling with LOD selection, mesh-shader raster,
   forward shading unchanged. RT keeps full-resolution BLAS.
2. **RT on clusters**: cluster acceleration structures built from the same
   clusters and the same LOD cut as the raster; partitioned TLAS.
3. **Foliage**: coverage-preserving simplification for disconnected
   geometry (leaves), opacity micromaps for masked clusters, far-field
   volumetric/impostor representation.
4. **Visibility buffer** (+ software raster for micro-triangles if
   measurements call for it): one geometry pass, material resolve in
   compute; feeds DLSS guides.
5. **Hierarchical traversal, compression, streaming**: BVH over cluster
   groups with persistent-thread traversal (replaces the flat cluster test),
   quantized vertices, paged cluster streaming.

Each slice records its choices in `docs/decisions.md`.

## Slice 1: cluster LOD raster

### Goals

- Every opaque and alpha-masked mesh renders through clusters with
  continuous LOD; triangle count on screen follows screen-space error
  (target 1 pixel), not source triangle count.
- No cracks between clusters of different LOD (DAG cut rule), no visible
  popping beyond what a 1-pixel error allows.
- Cluster-level frustum and occlusion culling, conservative (no visible
  cluster skipped, no disocclusion popping).
- Same image as today at LOD error 0 (full detail), within capture noise.
- Faster than the current per-instance path in the scatter test scene.

### Non-goals (later slices)

- Cluster acceleration structures (slice 2): BLAS stays full-res, built
  from the original indices.
- Visibility buffer / software raster (slice 4).
- Hierarchical traversal, compression, streaming (slice 5).
- Blended geometry: stays on the current CPU-sorted indexed path.

### Approaches

- **Raster:**
  1. **Mesh shaders (chosen).** `VK_EXT_mesh_shader` is cross-vendor
     (NVIDIA Turing+, AMD RDNA2+, Intel Arc). One mesh workgroup per
     visible cluster reads the cluster's vertex list and micro-indices
     directly; no index buffer expansion, no extra memory traffic.
     Matches what Nanite, niagara and vk_lod_clusters do for hardware
     raster.
  2. Compute-expanded index buffer + `vkCmdDrawIndexedIndirectCount`.
     Works everywhere, benchmarks comparable (williscool), but writes and
     re-reads indices every frame and needs a worst-case-sized buffer.
  3. Software raster for everything. Only wins for micro-triangles; it's
     the slice-4 question.
- **LOD build:**
  1. **meshoptimizer 1.0 `clusterlod.h` at load, cached on disk
     (chosen).** Maintained, used by the
     industry, handles billions of triangles in minutes, implements the
     Nanite group/partition/simplify loop with permissive and sloppy
     fallbacks. Writing our own is no better for slice 1; slice 3 will
     modify the simplification step for foliage.
  2. Discrete per-mesh LODs: rejected by the phase plan (can't make one
     7M-triangle tree cheap up close and far away at once).
- **Cluster occlusion:**
  1. **Nanite's scheme (chosen):** phase 0 tests clusters against *last
     frame's* depth pyramid with last frame's view; clusters it rejects
     are queued and retested in phase 1 against the pyramid built from
     phase 0's depth. No per-cluster visibility storage, conservative.
  2. niagara-style per-cluster visibility bits: memory grows with
     instances x clusters; rejected.

### Design

**Build and cache.** `loadGltf` builds the DAG per primitive with
`clodBuild` (128 triangles / 128 vertices per cluster, default raster
config; meshoptimizer compiled `-O2 -fno-sanitize=all`, like Basis) and
caches the result next to the glTF (`<name>.clusters`, a derived-data
cache like Unreal's DDC), keyed by a format version and the source's size
and modification time; a stale or mismatched cache is rebuilt. Building in
the loader rather than in `meridian-cook` keeps one code path and
guarantees the clusters index exactly the vertices the loader produces
(MikkTSpace may change them); cooking can pre-warm the cache later.
`MeshData` gains the cluster arrays:

- per cluster: bounds sphere (culling), own-group LOD bounds (center,
  radius, error), refined-group LOD bounds (error = 0 sentinel when
  `refined == -1`), first/count into a vertex-index list (uint32, indexes
  the mesh's vertices), first/count into micro-indices (uint8 triplets).

Vertices stay the 48-byte `Vertex` in the shared pool (simplified clusters
reference a subset of the original vertices). Original indices stay too:
the BLAS (slice 1) and blended meshes use them.

**GPU tables.** Cluster pool (`ClusterInfo`, must-match), vertex-index
pool, micro-index pool, all bump-allocated like the vertex pool and reached
through `SceneData` addresses. `MeshInfo` gains first cluster and cluster
count.

**LOD rule (Nanite, clusterlod's form).** With projected error
`e / max(distance(center, camera) - radius, near) * proj[1][1] * 0.5 *
screenHeight` (pixels, uniform-scale-aware: error and radius times the
instance's max axis scale), render a cluster if its own group's
simplified error projects *over* the threshold and its refined group's
error projects *at or under* it (or it is original geometry). Errors and
bounds are monotonic along the DAG (clusterlod guarantees it), so the
selected set is a crack-free cut. Threshold default 1 pixel,
`Renderer::setLodErrorPixels(float)`; game flag `--lod-error PX` (0 =
full detail, for A/B).

**Culling, two levels, two phases.**

- Instance cull (existing `cull.comp`, both phases) stays, but instead of
  draw commands it appends *cluster work items* `(instance, first cluster,
  count <= 64)` covering the instance's clusters to a work list, with an
  indirect dispatch count.
- Cluster cull (new compute, one thread per cluster, indirect dispatch
  over work items): LOD rule, frustum test, occlusion test. Phase 0:
  against last frame's pyramid and view; rejected-by-occlusion clusters
  are appended to a retest list. Phase 1: retests that list plus the work
  items of newly visible instances against the new pyramid. Survivors
  append `(instance, cluster)` to one of the 4 draw lists
  (opaque/masked x single/double-sided).
- The pyramid is not rebuilt at the end of the frame: last frame's early
  pyramid is conservative (fewer occluders), costs nothing. It is only
  valid for static instances (true today: instances never move) and only
  after one complete frame at the current size; until then phase 0 skips
  the occlusion test (everything in the frustum draws early).

**Raster.** `mesh.mesh` (new; `mesh.vert` retires for clusters, stays for
blended) reads `(instance, cluster)`, outputs up to 128 vertices / 128
triangles with the same varyings as `mesh.vert`, so `mesh.frag` and the
masked variant are unchanged. Draw with
`vkCmdDrawMeshTasksIndirectCountEXT`, one task-free mesh workgroup per
list entry (groupCountX = entries, 2D split over the device limit).
Pipelines: mesh + frag per bucket as now, plus the blended vertex
pipeline.

**Stats.** Drawn instances stays; add drawn clusters and drawn triangles
(GPU counters, read back like today), printed by capture runs.

**Test asset.** One Poly Haven film-weight tree in
`fetch-sample-assets.sh` (the asset class that motivated this phase) and
`--scatter` able to place it.

### Verification

- Unit tests: cluster cache round-trips; LOD bounds monotonic for every
  cluster (refined-group error <= own-group error, refined sphere inside
  own sphere); CPU replica of the LOD rule on a test mesh selects a cut
  where every original triangle's region is covered once (sum of selected
  cluster areas within a few % of the original surface area at several
  thresholds).
- Captures: `--lod-error 0` vs current main at the default and overview
  views (mean difference within noise); `--lod-error 1` vs 0 (small
  difference, no cracks: inspect at the overview); motion capture for
  popping; scatter 3000 GPU ms vs main (median of 5, same resolution);
  triangles drawn at the overview vs total.
- 0 validation errors, no leaks.

### Left to the builder

Cache byte layout, workgroup sizes, work-item chunk size, the exact
mesh-shader output limits (within the device's
`maxMeshOutputVertices/Primitives`), cluster list capacities (sized for
the scatter scene with headroom; overflow drops clusters and is counted).

### Approval

Direction (differentiators, slice order, NVIDIA-first RT) agreed with the
user on 2026-11-19; the user delegated the rest of the design and
autonomous execution overnight ("go on ... expand autonomously considering
the goals and our direction"). Calls made without them: every "chosen"
above, the 1-pixel default, cluster size 128, the cache file format.

### Sources

- Karis, Nanite: A Deep Dive (SIGGRAPH 2021)
- meshoptimizer 1.0 clusterlod: https://github.com/zeux/meshoptimizer/blob/v1.0/demo/clusterlod.h
- zeux, Billions of triangles in minutes: https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/
- nvpro-samples vk_lod_clusters (cluster LOD raster + RT, Vulkan)
- DXR clustered geometry: https://www.techpowerup.com/354281/directx-raytracing-gets-clustered-geometry-for-higher-fps-and-up-to-90-vram-savings
- UE 5.7 Nanite foliage: https://80.lv/articles/get-a-glimpse-of-nanite-foliage-with-voxel-representation-in-ue5-7
- williscool task/mesh benchmarks: https://www.williscool.com/technical/task-mesh-benchmarking/task-mesh-benchmarking.md.html
