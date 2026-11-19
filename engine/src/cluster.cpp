#include <meridian/cluster.h>
#include <meridian/mesh.h>

#include <meshoptimizer.h>
#include <clusterlod.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace meridian {

namespace {

constexpr size_t kMaxClusterTriangles = 128;
constexpr char kCacheMagic[4] = {'M', 'C', 'L', 'D'};
// Bump when the build settings or Cluster's layout change.
constexpr uint32_t kCacheVersion = 1;

template <typename T> void writeArray(std::ofstream &out, const std::vector<T> &v) {
  const uint64_t count = v.size();
  out.write(reinterpret_cast<const char *>(&count), sizeof(count));
  out.write(reinterpret_cast<const char *>(v.data()),
            static_cast<std::streamsize>(count * sizeof(T)));
}

template <typename T>
bool readArray(std::ifstream &in, uint64_t bytesLeft, std::vector<T> &v) {
  uint64_t count = 0;
  if (!in.read(reinterpret_cast<char *>(&count), sizeof(count)) ||
      count > bytesLeft / sizeof(T)) {
    return false;
  }
  v.resize(count);
  return static_cast<bool>(in.read(reinterpret_cast<char *>(v.data()),
                                   static_cast<std::streamsize>(count * sizeof(T))));
}

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

void writeClusterCache(const std::filesystem::path &file, uint64_t sourceKey,
                       const std::vector<ClusterLod> &lods) {
  // Written aside and renamed, so a crash never leaves a half file behind.
  const std::filesystem::path partial = file.string() + ".partial";
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    out.write(kCacheMagic, sizeof(kCacheMagic));
    out.write(reinterpret_cast<const char *>(&kCacheVersion), sizeof(kCacheVersion));
    out.write(reinterpret_cast<const char *>(&sourceKey), sizeof(sourceKey));
    const uint64_t count = lods.size();
    out.write(reinterpret_cast<const char *>(&count), sizeof(count));
    for (const ClusterLod &lod : lods) {
      writeArray(out, lod.clusters);
      writeArray(out, lod.vertices);
      writeArray(out, lod.triangles);
    }
    if (!out) {
      throw std::runtime_error("cannot write " + partial.string());
    }
  }
  std::filesystem::rename(partial, file);
}

std::optional<std::vector<ClusterLod>>
readClusterCache(const std::filesystem::path &file, uint64_t sourceKey) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(file, error);
  std::ifstream in(file, std::ios::binary);
  if (error || !in) {
    return std::nullopt;
  }
  char magic[4] = {};
  uint32_t version = 0;
  uint64_t key = 0;
  uint64_t count = 0;
  in.read(magic, sizeof(magic));
  in.read(reinterpret_cast<char *>(&version), sizeof(version));
  in.read(reinterpret_cast<char *>(&key), sizeof(key));
  in.read(reinterpret_cast<char *>(&count), sizeof(count));
  if (!in || std::memcmp(magic, kCacheMagic, sizeof(magic)) != 0 ||
      version != kCacheVersion || key != sourceKey || count > size) {
    return std::nullopt;
  }
  std::vector<ClusterLod> lods(count);
  for (ClusterLod &lod : lods) {
    if (!readArray(in, size, lod.clusters) || !readArray(in, size, lod.vertices) ||
        !readArray(in, size, lod.triangles)) {
      return std::nullopt;
    }
  }
  return lods;
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
