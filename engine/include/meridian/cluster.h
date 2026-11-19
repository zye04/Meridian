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

struct ClusterLod {
  std::vector<Cluster> clusters;
  // Per cluster: indices into the mesh's vertices.
  std::vector<uint32_t> vertices;
  // Per cluster: 3 cluster-local vertex indices per triangle.
  std::vector<uint8_t> triangles;
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

// Derived-data cache of every primitive's hierarchy in one file. sourceKey
// identifies the source it was built from; any other key, version or a
// short file reads as nullopt.
void writeClusterCache(const std::filesystem::path &file, uint64_t sourceKey,
                       const std::vector<ClusterLod> &lods);
std::optional<std::vector<ClusterLod>>
readClusterCache(const std::filesystem::path &file, uint64_t sourceKey);

} // namespace meridian
