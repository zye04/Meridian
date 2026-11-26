#pragma once

#include <meridian/math/vec3.h>
#include <meridian/voxelize.h>

#include <cstdint>
#include <span>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace meridian {

struct MeshData;

// A bounding sphere with the simplification error of the geometry inside it,
// all in mesh space.
struct LodBounds {
  Vec3 center;
  float radius = 0.0f;
  float error = 0.0f;
};

// Up to 128 triangles of one level of a mesh's cluster LOD hierarchy
// (Nanite-style DAG).
struct Cluster {
  // Culling sphere around the cluster's own triangles.
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  // The coarser version that replaces this cluster's group (error FLT_MAX
  // when there is none).
  LodBounds self;
  // The finer group this cluster was simplified from (error -1 for original
  // geometry).
  LodBounds refined;
  uint32_t firstVertex = 0;
  uint32_t vertexCount = 0;
  uint32_t firstTriangle = 0;
  uint32_t triangleCount = 0;
  // A voxel cluster (brickCount > 0, no triangles): bricks firstBrick.. of
  // ClusterLod::bricks on the lattice of edge voxelEdge (voxelize.h).
  uint32_t firstBrick = 0;
  uint32_t brickCount = 0;
  float voxelEdge = 0.0f;
  // The group this cluster is a member of (replaced together by `self`) and
  // the group it was made from (-1 for original geometry).
  uint32_t group = 0;
  int32_t refinedGroup = -1;
};

// A node of a BVH over one DAG level's cluster groups. Leaves are groups:
// their children are a range of clusters; an inner node's children are a
// range of nodes after it. 48 bytes, no padding, so the GPU can read it.
struct ClusterBvhNode {
  // Encloses the self and refined spheres of every cluster below, with the
  // largest self error: when it projects at or under the threshold, no
  // cluster below can be drawn.
  LodBounds lod;
  // Encloses the culling spheres of every cluster below.
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  uint32_t firstChild = 0;
  uint32_t childCount = 0;
  uint32_t leaf = 0;
};

// One DAG level: its BVH root and what bounds where the level can hold the
// cut (minError: smallest refined error, -1 when the level has original
// geometry; maxError: largest self error; minGroupRadius: smallest refined
// sphere radius).
struct ClusterLevelInfo {
  uint32_t rootNode = 0;
  float minError = 0.0f;
  float maxError = 0.0f;
  float minGroupRadius = 0.0f;
};

struct ClusterLod {
  std::vector<Cluster> clusters;
  // Per cluster: indices into the mesh's vertices.
  std::vector<uint32_t> vertices;
  // Per cluster: 3 cluster-local vertex indices per triangle.
  std::vector<uint8_t> triangles;
  std::vector<ClusterBvhNode> nodes;
  std::vector<ClusterLevelInfo> levels;
  // Leaf meshes: vertices of coarser levels moved to keep the leaves' area
  // (scaled about each leaf's centre). Cluster vertex index mesh vertex count
  // + i is a copy of mesh vertex addedSources[i] at addedPositions[i];
  // appendClusterVertices adds them to the mesh.
  std::vector<uint32_t> addedSources;
  std::vector<Vec3> addedPositions;
  // The voxel clusters' bricks and voxels, and per voxel the optical depth
  // from its centre out of the model along +x, -x, +y, -y, +z, -z (4 bits
  // each, steps of kVoxelAxisDepthStep; bakeModelAxisDepths).
  std::vector<VoxelBrickRef> bricks;
  std::vector<PackedVoxel> voxels;
  std::vector<uint32_t> voxelAxis;
  // A voxelizing mesh's whole surface voxelized at kFarTemplateVoxel: the
  // far field's template (farBakeGridFromVoxels, farfield.h).
  std::vector<VoxelBrickRef> farBricks;
  std::vector<PackedVoxel> farVoxels;
};

constexpr float kVoxelAxisDepthStep = 0.5f;
// A voxel this deep in its model from the camera shows through less than 3%
// of the time (voxel_lod.glsl's kVoxelHiddenDepth): voxel_bricks.comp skips
// it, and bakeModelAxisDepths drops voxels this deep from every direction.
constexpr float kVoxelHiddenDepth = 3.5f;

// What a mesh's voxel levels are made of: its material, by index into
// materials (images decoded on the CPU).
struct VoxelSource {
  std::span<const VoxelMaterial> materials;
  uint32_t material = 0;
};

// With voxels (and mesh.voxelize), a group becomes voxel clusters once its
// voxels are no worse than its simplified triangles: the lattice edge is the
// largest power of two at or under the triangles' error, not under the
// height / 512 of the mesh or its model (voxelModelHeight, the larger), and
// their error at least the children's. A solid part (voxelModelHeight > 0,
// bark) waits until it is thinner than that edge: most of the group's voxels
// at it under kOpaqueVoxelFloor along every axis. Above, one voxel hierarchy
// per mesh: each level groups the clusters of a twice-as-large region,
// whichever group they came from, and doubles the edge (combineVoxelGrids,
// mergeVoxelGrid).
ClusterLod buildClusterLod(const MeshData &mesh, const VoxelSource *voxels = nullptr);

// First folds each opaque voxel that shares its cell (same edge) with a
// transmissive one into it (combineVoxels): a branch inside leaves blends
// into them instead of drawing as a cube of its own. Then fills every
// ClusterLod::voxelAxis of one model's meshes: the optical depth
// through all of the model's voxels at the same edge, from each voxel's
// centre out along the six axis directions. Then drops the voxels deeper
// than kVoxelHiddenDepth along all six (no view sees them, the sun's
// included), and the bricks that leave empty.
void bakeModelAxisDepths(std::vector<ClusterLod> &meshes);

// Simplification error in pixels on a screen `screenHeight` pixels tall,
// for bounds already placed in world space and uniformly scaled by `scale`
// (projY: projection[1][1]). Ignores perspective distortion (clusterlod's
// estimate). Must match projectedError in cull.glsl. With
// orthoPixelsPerMeter > 0 (an orthographic view) the error is
// error * scale * orthoPixelsPerMeter and the camera, projY and
// screenHeight are ignored.
float projectedErrorPixels(const LodBounds &bounds, Vec3 camera, float scale,
                           float projY, float nearPlane, float screenHeight,
                           float orthoPixelsPerMeter = 0.0f);

// Voxel clusters give way to coarser voxels at this multiple of the LOD
// threshold (pixels per voxel edge at the switch): coverage-built foliage
// fills its crown's volume, so finer voxels cost the cube. Must match
// common.glsl.
inline constexpr float kVoxelPixels = 2.0f;

// The pixel thresholds of a cluster's two LOD tests: its self (coarser
// replacement) and its refined (finer) bounds.
struct LodThresholds {
  float self = 0.0f;
  float refined = 0.0f;
};

// Per cluster, the thresholds the GPU tests it with (cluster_cull.comp):
// a triangle cluster {T, T}; a voxel cluster self kVoxelPixels T, refined
// T where it replaces triangles and kVoxelPixels T where it replaces voxels.
// The upload derives the GPU's flag for the refined side from this.
std::vector<LodThresholds> clusterThresholds(const ClusterLod &lod, float threshold);

// Nanite's cut: draw a cluster when its coarser replacement is too coarse
// and it is itself fine enough. Errors grow monotonically up the DAG, so the
// drawn clusters cover the surface once, without cracks.
bool lodVisible(const Cluster &cluster, Vec3 camera, float scale, float projY,
                float nearPlane, float screenHeight, LodThresholds thresholds,
                float orthoPixelsPerMeter = 0.0f);

// CPU replica of the GPU traversal: the levels whose band can hold the cut,
// then their BVHs breadth-first, skipping nodes whose bounds rule every
// cluster below out, then lodVisible per cluster of the reached leaves
// (clusterThresholds). Returns exactly the clusters lodVisible accepts, in
// traversal order. Without voxels (voxelsAt false) no voxel cluster draws
// and the triangles they replace never give way, as on the GPU.
std::vector<uint32_t> selectClustersBvh(const ClusterLod &lod, Vec3 camera,
                                        float scale, float projY,
                                        float nearPlane, float screenHeight,
                                        float thresholdPixels,
                                        float orthoPixelsPerMeter = 0.0f,
                                        bool voxels = true);

// Whether voxel clusters may replace an instance's triangles in the main
// view: beyond voxelNear meters of the camera (common.glsl voxelLodAt).
bool voxelsAt(Vec3 instanceOrigin, Vec3 camera, float voxelNear);

// Derived-data cache of every primitive's hierarchy in one file. sourceKey
// identifies the source it was built from; any other key, version or a
// short file reads as nullopt.
// The cache file's extension, carrying the cache and voxelizer versions, so
// builds with different versions sharing one assets/ keep separate files
// instead of rebuilding each other's (e.g. ".v00080406.10.clusters").
std::string clusterCacheExtension();
uint32_t clusterCacheVersion();
uint32_t voxelizerVersion();
void writeClusterCache(const std::filesystem::path &file, uint64_t sourceKey,
                       const std::vector<ClusterLod> &lods);
std::optional<std::vector<ClusterLod>>
readClusterCache(const std::filesystem::path &file, uint64_t sourceKey);

// Whether every cluster stays inside its own arrays, the mesh shader's
// limits and the mesh's vertices: a damaged cache must not reach the GPU.
bool validClusterLod(const ClusterLod &lod, size_t meshVertexCount);

} // namespace meridian
