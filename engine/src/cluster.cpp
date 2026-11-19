#include <meridian/cluster.h>
#include <meridian/mesh.h>

#include <meshoptimizer.h>
#include <clusterlod.h>

#include <algorithm>
#include <cfloat>

namespace meridian {

namespace {

constexpr size_t kMaxClusterTriangles = 128;

LodBounds toLodBounds(const clodBounds &b) {
  return {{b.center[0], b.center[1], b.center[2]}, b.radius, b.error};
}

} // namespace

ClusterLod buildClusterLod(const MeshData &mesh) {
  // Normals guide simplification; UVs (the floats around them in Vertex:
  // uvX, normal, uvY) only protect seams.
  const float attributeWeights[5] = {0.0f, 0.5f, 0.5f, 0.5f, 0.0f};
  clodMesh input{};
  input.indices = mesh.indices.data();
  input.index_count = mesh.indices.size();
  input.vertex_count = mesh.vertices.size();
  input.vertex_positions = &mesh.vertices[0].position.x;
  input.vertex_positions_stride = sizeof(Vertex);
  input.vertex_attributes = &mesh.vertices[0].uvX;
  input.vertex_attributes_stride = sizeof(Vertex);
  input.attribute_weights = attributeWeights;
  input.attribute_count = 5;
  input.attribute_protect_mask = (1u << 0) | (1u << 4);

  ClusterLod lod;
  std::vector<LodBounds> groups;
  std::vector<int> refinedGroups;
  std::vector<unsigned int> localVertices(kMaxClusterTriangles * 3);
  std::vector<unsigned char> localTriangles(kMaxClusterTriangles * 3);
  clodBuild(clodDefaultConfig(kMaxClusterTriangles), input,
            [&](clodGroup group, const clodCluster *clusters, size_t count) {
              const auto groupIndex = static_cast<int>(groups.size());
              groups.push_back(toLodBounds(group.simplified));
              for (size_t i = 0; i < count; ++i) {
                const clodCluster &c = clusters[i];
                const size_t vertexCount =
                    clodLocalIndices(localVertices.data(), localTriangles.data(),
                                     c.indices, c.index_count);
                Cluster cluster;
                cluster.boundsCenter = {c.bounds.center[0], c.bounds.center[1],
                                        c.bounds.center[2]};
                cluster.boundsRadius = c.bounds.radius;
                cluster.self = groups.back();
                cluster.firstVertex = static_cast<uint32_t>(lod.vertices.size());
                cluster.vertexCount = static_cast<uint32_t>(vertexCount);
                cluster.firstTriangle =
                    static_cast<uint32_t>(lod.triangles.size() / 3);
                cluster.triangleCount = static_cast<uint32_t>(c.index_count / 3);
                lod.vertices.insert(lod.vertices.end(), localVertices.begin(),
                                    localVertices.begin() + vertexCount);
                lod.triangles.insert(lod.triangles.end(), localTriangles.begin(),
                                     localTriangles.begin() + c.index_count);
                lod.clusters.push_back(cluster);
                refinedGroups.push_back(c.refined);
              }
              return groupIndex;
            });

  for (size_t i = 0; i < lod.clusters.size(); ++i) {
    const int refined = refinedGroups[i];
    lod.clusters[i].refined =
        refined < 0 ? LodBounds{{}, 0.0f, -1.0f} : groups[refined];
  }
  return lod;
}

float projectedErrorPixels(const LodBounds &bounds, Vec3 camera, float scale,
                           float projY, float nearPlane, float screenHeight) {
  if (bounds.error == FLT_MAX) {
    return FLT_MAX;
  }
  const float distance = std::max(
      length(bounds.center - camera) - bounds.radius * scale, nearPlane);
  return bounds.error * scale / distance * projY * 0.5f * screenHeight;
}

bool lodVisible(const Cluster &cluster, Vec3 camera, float scale, float projY,
                float nearPlane, float screenHeight, float thresholdPixels) {
  const auto project = [&](const LodBounds &b) {
    return projectedErrorPixels(b, camera, scale, projY, nearPlane, screenHeight);
  };
  return project(cluster.self) > thresholdPixels &&
         (cluster.refined.error < 0.0f || project(cluster.refined) <= thresholdPixels);
}

} // namespace meridian
