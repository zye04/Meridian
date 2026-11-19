#pragma once

#include <meridian/math/vec3.h>

#include <cstdint>
#include <filesystem>
#include <optional>
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
};

ClusterLod buildClusterLod(const MeshData &mesh);

// Simplification error in pixels on a screen `screenHeight` pixels tall,
// for bounds already placed in world space and uniformly scaled by `scale`
// (projY: projection[1][1]). Ignores perspective distortion (clusterlod's
// estimate). Must match cluster_cull.comp.
float projectedErrorPixels(const LodBounds &bounds, Vec3 camera, float scale,
                           float projY, float nearPlane, float screenHeight);

// Nanite's cut: draw a cluster when its coarser replacement is too coarse
// and it is itself fine enough. Errors grow monotonically up the DAG, so the
// drawn clusters cover the surface once, without cracks.
bool lodVisible(const Cluster &cluster, Vec3 camera, float scale, float projY,
                float nearPlane, float screenHeight, float thresholdPixels);

// CPU replica of the GPU traversal: the levels whose band can hold the cut,
// then their BVHs breadth-first, skipping nodes whose bounds rule every
// cluster below out, then lodVisible per cluster of the reached leaves.
// Returns exactly the clusters lodVisible accepts, in traversal order.
std::vector<uint32_t> selectClustersBvh(const ClusterLod &lod, Vec3 camera,
                                        float scale, float projY,
                                        float nearPlane, float screenHeight,
                                        float thresholdPixels);

// Derived-data cache of every primitive's hierarchy in one file. sourceKey
// identifies the source it was built from; any other key, version or a
// short file reads as nullopt.
void writeClusterCache(const std::filesystem::path &file, uint64_t sourceKey,
                       const std::vector<ClusterLod> &lods);
std::optional<std::vector<ClusterLod>>
readClusterCache(const std::filesystem::path &file, uint64_t sourceKey);

// Whether every cluster stays inside its own arrays, the mesh shader's
// limits and the mesh's vertices: a damaged cache must not reach the GPU.
bool validClusterLod(const ClusterLod &lod, size_t meshVertexCount);

} // namespace meridian
