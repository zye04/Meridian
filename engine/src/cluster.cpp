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
// mesh.mesh's max_vertices; clodDefaultConfig caps clusters at it too.
constexpr size_t kMaxClusterVertices = 128;
constexpr char kCacheMagic[4] = {'M', 'C', 'L', 'D'};
// Bump the high half when the build settings or Cluster's layout change; the
// low half follows the meshoptimizer pin, whose output may change.
constexpr uint32_t kCacheVersion = (6u << 16) | MESHOPTIMIZER_VERSION;
// Most a leaf piece is scaled up (linearly) to keep its group's area.
constexpr float kMaxLeafScale = 4.0f;
constexpr size_t kBvhBranching = 8;
static_assert(sizeof(ClusterBvhNode) == 48 && sizeof(ClusterLevelInfo) == 16);

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

struct Sphere {
  Vec3 center;
  float radius = 0.0f;
};

// Around the box of the spheres, widened by 1e-4 so float rounding in the
// traversal's distance tests can't make a node look farther than a cluster
// below it.
Sphere enclose(const std::vector<Sphere> &spheres) {
  Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX};
  Vec3 hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (const Sphere &s : spheres) {
    lo = {std::min(lo.x, s.center.x - s.radius), std::min(lo.y, s.center.y - s.radius),
          std::min(lo.z, s.center.z - s.radius)};
    hi = {std::max(hi.x, s.center.x + s.radius), std::max(hi.y, s.center.y + s.radius),
          std::max(hi.z, s.center.z + s.radius)};
  }
  Sphere result{(lo + hi) * 0.5f, 0.0f};
  for (const Sphere &s : spheres) {
    result.radius = std::max(result.radius, length(s.center - result.center) + s.radius);
  }
  result.radius *= 1.0f + 1e-4f;
  return result;
}

struct GroupRange {
  int depth = 0;
  uint32_t firstCluster = 0;
  uint32_t clusterCount = 0;
};

ClusterBvhNode groupLeaf(const ClusterLod &lod, const GroupRange &group) {
  std::vector<Sphere> lodSpheres;
  std::vector<Sphere> cullSpheres;
  ClusterBvhNode leaf;
  for (uint32_t i = group.firstCluster; i < group.firstCluster + group.clusterCount; ++i) {
    const Cluster &c = lod.clusters[i];
    lodSpheres.push_back({c.self.center, c.self.radius});
    if (c.refined.error >= 0.0f) {
      lodSpheres.push_back({c.refined.center, c.refined.radius});
    }
    cullSpheres.push_back({c.boundsCenter, c.boundsRadius});
    leaf.lod.error = std::max(leaf.lod.error, c.self.error);
  }
  const Sphere lodSphere = enclose(lodSpheres);
  const Sphere cullSphere = enclose(cullSpheres);
  leaf.lod.center = lodSphere.center;
  leaf.lod.radius = lodSphere.radius;
  leaf.boundsCenter = cullSphere.center;
  leaf.boundsRadius = cullSphere.radius;
  leaf.firstChild = group.firstCluster;
  leaf.childCount = group.clusterCount;
  leaf.leaf = 1;
  return leaf;
}

// Median splits along the longest axis of the leaf centers until there are
// kBvhBranching parts or single leaves.
void splitLeaves(std::vector<uint32_t> &items, const std::vector<ClusterBvhNode> &leaves,
                 size_t begin, size_t end, size_t parts,
                 std::vector<std::pair<size_t, size_t>> &out) {
  if (parts == 1 || end - begin == 1) {
    out.emplace_back(begin, end);
    return;
  }
  const auto axis = [](Vec3 v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; };
  int longest = 0;
  float longestExtent = -1.0f;
  for (int a = 0; a < 3; ++a) {
    float lo = FLT_MAX;
    float hi = -FLT_MAX;
    for (size_t i = begin; i < end; ++i) {
      lo = std::min(lo, axis(leaves[items[i]].lod.center, a));
      hi = std::max(hi, axis(leaves[items[i]].lod.center, a));
    }
    if (hi - lo > longestExtent) {
      longest = a;
      longestExtent = hi - lo;
    }
  }
  const size_t mid = begin + (end - begin) / 2;
  std::nth_element(items.begin() + begin, items.begin() + mid, items.begin() + end,
                   [&](uint32_t a, uint32_t b) {
                     const float ka = axis(leaves[a].lod.center, longest);
                     const float kb = axis(leaves[b].lod.center, longest);
                     return ka < kb || (ka == kb && a < b);
                   });
  splitLeaves(items, leaves, begin, mid, parts / 2, out);
  splitLeaves(items, leaves, mid, end, parts / 2, out);
}

// One BVH per DAG level over that level's groups, top-down, nodes laid out
// breadth-first so every child comes after its parent.
void buildBvh(ClusterLod &lod, const std::vector<GroupRange> &groups) {
  std::vector<ClusterBvhNode> leaves;
  int maxDepth = -1;
  for (const GroupRange &g : groups) {
    leaves.push_back(groupLeaf(lod, g));
    maxDepth = std::max(maxDepth, g.depth);
  }
  for (int depth = 0; depth <= maxDepth; ++depth) {
    std::vector<uint32_t> items;
    ClusterLevelInfo level{static_cast<uint32_t>(lod.nodes.size()), FLT_MAX, 0.0f, FLT_MAX};
    for (uint32_t g = 0; g < groups.size(); ++g) {
      if (groups[g].depth != depth) {
        continue;
      }
      items.push_back(g);
      for (uint32_t i = groups[g].firstCluster;
           i < groups[g].firstCluster + groups[g].clusterCount; ++i) {
        const Cluster &c = lod.clusters[i];
        level.minError = std::min(level.minError, c.refined.error);
        level.maxError = std::max(level.maxError, c.self.error);
        level.minGroupRadius = std::min(level.minGroupRadius, c.refined.radius);
      }
    }
    if (items.empty()) {
      continue;
    }
    struct Pending {
      uint32_t node;
      size_t begin, end;
    };
    std::vector<Pending> queue = {{level.rootNode, 0, items.size()}};
    lod.nodes.emplace_back();
    for (size_t q = 0; q < queue.size(); ++q) {
      const Pending p = queue[q];
      if (p.end - p.begin == 1) {
        lod.nodes[p.node] = leaves[items[p.begin]];
        continue;
      }
      std::vector<std::pair<size_t, size_t>> parts;
      splitLeaves(items, leaves, p.begin, p.end, kBvhBranching, parts);
      lod.nodes[p.node].firstChild = static_cast<uint32_t>(lod.nodes.size());
      lod.nodes[p.node].childCount = static_cast<uint32_t>(parts.size());
      for (const auto &[begin, end] : parts) {
        queue.push_back({static_cast<uint32_t>(lod.nodes.size()), begin, end});
        lod.nodes.emplace_back();
      }
    }
    for (size_t n = lod.nodes.size(); n-- > level.rootNode;) {
      ClusterBvhNode &node = lod.nodes[n];
      if (node.leaf) {
        continue;
      }
      std::vector<Sphere> lodSpheres;
      std::vector<Sphere> cullSpheres;
      for (uint32_t i = node.firstChild; i < node.firstChild + node.childCount; ++i) {
        const ClusterBvhNode &child = lod.nodes[i];
        lodSpheres.push_back({child.lod.center, child.lod.radius});
        cullSpheres.push_back({child.boundsCenter, child.boundsRadius});
        node.lod.error = std::max(node.lod.error, child.lod.error);
      }
      const Sphere lodSphere = enclose(lodSpheres);
      const Sphere cullSphere = enclose(cullSpheres);
      node.lod.center = lodSphere.center;
      node.lod.radius = lodSphere.radius;
      node.boundsCenter = cullSphere.center;
      node.boundsRadius = cullSphere.radius;
    }
    lod.levels.push_back(level);
  }
}

} // namespace

void appendClusterVertices(MeshData &mesh) {
  const size_t count = mesh.clusters.addedSources.size();
  mesh.vertices.reserve(mesh.vertices.size() + count);
  for (size_t i = 0; i < count; ++i) {
    Vertex v = mesh.vertices[mesh.clusters.addedSources[i]];
    v.position = mesh.clusters.addedPositions[i];
    mesh.vertices.push_back(v);
  }
}

namespace {

uint32_t findRoot(std::vector<uint32_t> &parent, uint32_t i) {
  while (parent[i] != i) {
    i = parent[i] = parent[parent[i]];
  }
  return i;
}

// Simplification shrinks and drops disjoint leaves, thinning a crown with
// distance. Each group's simplified clusters are scaled about the centres of
// their connected pieces so they cover the area their group had as drawn
// (stochastic simplification's area rule, Cook et al. 2007, per DAG level).
// Groups are numbered in build order, so a cluster's refined group, and its
// scale, come before the group the cluster belongs to.
void keepLeafArea(const MeshData &mesh, ClusterLod &lod, const std::vector<int> &groupOf,
                  const std::vector<int> &refinedOf, std::vector<LodBounds> &groups) {
  const size_t groupCount = groups.size();
  const auto clusterVertex = [&](const Cluster &c, uint32_t t, int k) {
    return lod.vertices[c.firstVertex + lod.triangles[(c.firstTriangle + t) * 3 + k]];
  };
  std::vector<double> rawArea(lod.clusters.size(), 0.0);
  for (size_t i = 0; i < lod.clusters.size(); ++i) {
    const Cluster &c = lod.clusters[i];
    for (uint32_t t = 0; t < c.triangleCount; ++t) {
      const Vec3 a = mesh.vertices[clusterVertex(c, t, 0)].position;
      rawArea[i] += 0.5 * length(cross(mesh.vertices[clusterVertex(c, t, 1)].position - a,
                                       mesh.vertices[clusterVertex(c, t, 2)].position - a));
    }
  }
  std::vector<std::vector<size_t>> members(groupCount), simplified(groupCount);
  for (size_t i = 0; i < lod.clusters.size(); ++i) {
    members[groupOf[i]].push_back(i);
    if (refinedOf[i] >= 0) {
      simplified[refinedOf[i]].push_back(i);
    }
  }
  std::vector<float> scale(groupCount, 1.0f);
  const auto drawnScale = [&](size_t i) { return refinedOf[i] < 0 ? 1.0f : scale[refinedOf[i]]; };
  for (size_t g = 0; g < groupCount; ++g) {
    double before = 0.0, after = 0.0;
    for (size_t i : members[g]) {
      before += rawArea[i] * drawnScale(i) * drawnScale(i);
    }
    for (size_t i : simplified[g]) {
      after += rawArea[i];
    }
    if (after > 0.0) {
      scale[g] = std::clamp(static_cast<float>(std::sqrt(before / after)), 1.0f, kMaxLeafScale);
    }
  }

  std::vector<uint32_t> parent(mesh.vertices.size());
  std::vector<Vec3> weighted(mesh.vertices.size());
  std::vector<double> weight(mesh.vertices.size());
  std::vector<uint32_t> added(mesh.vertices.size(), UINT32_MAX);
  std::vector<float> moved(groupCount, 0.0f);
  for (size_t g = 0; g < groupCount; ++g) {
    if (scale[g] == 1.0f) {
      continue;
    }
    // Pieces of the simplified clusters, by shared vertices, and their
    // area-weighted centres.
    for (size_t i : simplified[g]) {
      const Cluster &c = lod.clusters[i];
      for (uint32_t v = 0; v < c.vertexCount; ++v) {
        const uint32_t id = lod.vertices[c.firstVertex + v];
        parent[id] = id;
        weighted[id] = {};
        weight[id] = 0.0;
        added[id] = UINT32_MAX;
      }
    }
    for (size_t i : simplified[g]) {
      const Cluster &c = lod.clusters[i];
      for (uint32_t t = 0; t < c.triangleCount; ++t) {
        const uint32_t a = findRoot(parent, clusterVertex(c, t, 0));
        parent[findRoot(parent, clusterVertex(c, t, 1))] = a;
        parent[findRoot(parent, clusterVertex(c, t, 2))] = a;
      }
    }
    for (size_t i : simplified[g]) {
      const Cluster &c = lod.clusters[i];
      for (uint32_t t = 0; t < c.triangleCount; ++t) {
        const Vec3 a = mesh.vertices[clusterVertex(c, t, 0)].position;
        const Vec3 b = mesh.vertices[clusterVertex(c, t, 1)].position;
        const Vec3 d = mesh.vertices[clusterVertex(c, t, 2)].position;
        const float area = 0.5f * length(cross(b - a, d - a));
        const uint32_t root = findRoot(parent, clusterVertex(c, t, 0));
        weighted[root] = weighted[root] + (a + b + d) * (area / 3.0f);
        weight[root] += area;
      }
    }
    for (size_t i : simplified[g]) {
      Cluster &c = lod.clusters[i];
      const Vec3 center = c.boundsCenter;
      for (uint32_t v = 0; v < c.vertexCount; ++v) {
        uint32_t &id = lod.vertices[c.firstVertex + v];
        if (added[id] == UINT32_MAX) {
          const uint32_t root = findRoot(parent, id);
          const Vec3 pieceCenter =
              weight[root] > 0.0 ? weighted[root] * static_cast<float>(1.0 / weight[root])
                                 : mesh.vertices[id].position;
          added[id] = static_cast<uint32_t>(mesh.vertices.size() + lod.addedSources.size());
          lod.addedSources.push_back(id);
          lod.addedPositions.push_back(pieceCenter +
                                       (mesh.vertices[id].position - pieceCenter) * scale[g]);
        }
        const Vec3 position = lod.addedPositions[added[id] - mesh.vertices.size()];
        moved[g] = std::max(moved[g], length(position - mesh.vertices[lod.addedSources[added[id] - mesh.vertices.size()]].position));
        id = added[id];
        c.boundsRadius = std::max(c.boundsRadius, length(position - center));
      }
    }
  }

  // Scaling moves the leaves off the simplified surface, so the move counts
  // as simplification error: a scaled level is drawn only where the move
  // projects under the LOD threshold. Errors stay monotonic up the DAG.
  for (size_t g = 0; g < groupCount; ++g) {
    groups[g].error += moved[g];
    groups[g].radius += moved[g];
    for (size_t i : members[g]) {
      if (refinedOf[i] >= 0) {
        groups[g].error = std::max(groups[g].error, groups[refinedOf[i]].error);
      }
    }
  }
  for (size_t i = 0; i < lod.clusters.size(); ++i) {
    lod.clusters[i].self = groups[groupOf[i]];
    if (refinedOf[i] >= 0) {
      lod.clusters[i].refined = groups[refinedOf[i]];
    }
  }
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
  std::vector<GroupRange> groupRanges;
  std::vector<int> refinedGroups, clusterGroups;
  std::vector<unsigned int> localVertices(kMaxClusterTriangles * 3);
  std::vector<unsigned char> localTriangles(kMaxClusterTriangles * 3);
  clodBuild(clodDefaultConfig(kMaxClusterTriangles), input,
            [&](clodGroup group, const clodCluster *clusters, size_t count) {
              const auto groupIndex = static_cast<int>(groups.size());
              groups.push_back(toLodBounds(group.simplified));
              groupRanges.push_back({group.depth,
                                     static_cast<uint32_t>(lod.clusters.size()),
                                     static_cast<uint32_t>(count)});
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
                clusterGroups.push_back(groupIndex);
              }
              return groupIndex;
            });

  for (size_t i = 0; i < lod.clusters.size(); ++i) {
    const int refined = refinedGroups[i];
    lod.clusters[i].refined =
        refined < 0 ? LodBounds{{}, 0.0f, -1.0f} : groups[refined];
  }
  if (mesh.leaf) {
    keepLeafArea(mesh, lod, clusterGroups, refinedGroups, groups);
  }
  buildBvh(lod, groupRanges);
  return lod;
}

void writeClusterCache(const std::filesystem::path &file, uint64_t sourceKey,
                       const std::vector<ClusterLod> &lods) {
  // Written aside and renamed, so a crash never leaves a half file behind.
  const std::filesystem::path partial = file.string() + ".partial";
  std::error_code error;
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
      writeArray(out, lod.nodes);
      writeArray(out, lod.levels);
      writeArray(out, lod.addedSources);
      writeArray(out, lod.addedPositions);
    }
    if (!out) {
      std::filesystem::remove(partial, error);
      throw std::runtime_error("cannot write " + partial.string());
    }
  }
  std::filesystem::rename(partial, file, error);
  if (error) {
    std::filesystem::remove(partial, error);
    throw std::runtime_error("cannot write " + file.string());
  }
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
        !readArray(in, size, lod.triangles) || !readArray(in, size, lod.nodes) ||
        !readArray(in, size, lod.levels) || !readArray(in, size, lod.addedSources) ||
        !readArray(in, size, lod.addedPositions)) {
      return std::nullopt;
    }
  }
  return lods;
}

bool validClusterLod(const ClusterLod &lod, size_t meshVertexCount) {
  for (const Cluster &c : lod.clusters) {
    if (c.vertexCount > kMaxClusterVertices || c.triangleCount > kMaxClusterTriangles ||
        uint64_t{c.firstVertex} + c.vertexCount > lod.vertices.size() ||
        (uint64_t{c.firstTriangle} + c.triangleCount) * 3 > lod.triangles.size()) {
      return false;
    }
    for (uint32_t t = 0; t < c.triangleCount * 3; ++t) {
      if (lod.triangles[c.firstTriangle * 3 + t] >= c.vertexCount) {
        return false;
      }
    }
  }
  if (!lod.clusters.empty() && (lod.nodes.empty() || lod.levels.empty())) {
    return false;
  }
  std::vector<uint8_t> leafCount(lod.clusters.size(), 0);
  for (size_t n = 0; n < lod.nodes.size(); ++n) {
    const ClusterBvhNode &node = lod.nodes[n];
    const uint64_t end = uint64_t{node.firstChild} + node.childCount;
    // Inner children after their parent: the traversal always terminates.
    if (node.childCount == 0 || node.leaf > 1 ||
        (node.leaf ? end > lod.clusters.size()
                   : node.firstChild <= n || end > lod.nodes.size())) {
      return false;
    }
    for (uint64_t i = node.firstChild; node.leaf && i < end; ++i) {
      if (++leafCount[i] > 1) {
        return false;
      }
    }
  }
  if (std::find(leafCount.begin(), leafCount.end(), 0) != leafCount.end()) {
    return false;
  }
  for (const ClusterLevelInfo &level : lod.levels) {
    if (level.rootNode >= lod.nodes.size()) {
      return false;
    }
  }
  if (lod.addedSources.size() != lod.addedPositions.size() ||
      !std::all_of(lod.addedSources.begin(), lod.addedSources.end(),
                   [&](uint32_t v) { return v < meshVertexCount; })) {
    return false;
  }
  return std::all_of(lod.vertices.begin(), lod.vertices.end(), [&](uint32_t v) {
    return v < meshVertexCount + lod.addedSources.size();
  });
}

float projectedErrorPixels(const LodBounds &bounds, Vec3 camera, float scale,
                           float projY, float nearPlane, float screenHeight,
                           float orthoPixelsPerMeter) {
  if (bounds.error == FLT_MAX) {
    return FLT_MAX;
  }
  if (orthoPixelsPerMeter > 0.0f) {
    return bounds.error * scale * orthoPixelsPerMeter;
  }
  const float distance = std::max(
      length(bounds.center - camera) - bounds.radius * scale, nearPlane);
  return bounds.error * scale / distance * projY * 0.5f * screenHeight;
}

bool lodVisible(const Cluster &cluster, Vec3 camera, float scale, float projY,
                float nearPlane, float screenHeight, float thresholdPixels,
                float orthoPixelsPerMeter) {
  const auto project = [&](const LodBounds &b) {
    return projectedErrorPixels(b, camera, scale, projY, nearPlane, screenHeight,
                                orthoPixelsPerMeter);
  };
  return project(cluster.self) > thresholdPixels &&
         (cluster.refined.error < 0.0f || project(cluster.refined) <= thresholdPixels);
}

std::vector<uint32_t> selectClustersBvh(const ClusterLod &lod, Vec3 camera,
                                        float scale, float projY,
                                        float nearPlane, float screenHeight,
                                        float thresholdPixels,
                                        float orthoPixelsPerMeter) {
  const auto project = [&](const LodBounds &b) {
    return projectedErrorPixels(b, camera, scale, projY, nearPlane, screenHeight,
                                orthoPixelsPerMeter);
  };
  std::vector<uint32_t> queue;
  for (const ClusterLevelInfo &level : lod.levels) {
    const ClusterBvhNode &root = lod.nodes[level.rootNode];
    // Every refined sphere of the level lies inside the root's LOD sphere,
    // so none is farther than this; the band ends where even the smallest
    // refined error projects over the threshold from there.
    const float farthest =
        length(root.lod.center - camera) + (root.lod.radius - level.minGroupRadius) * scale;
    const float finest =
        orthoPixelsPerMeter > 0.0f
            ? level.minError * scale * orthoPixelsPerMeter
            : level.minError * scale / std::max(farthest, nearPlane) * projY * 0.5f *
                  screenHeight;
    if (finest <= thresholdPixels &&
        project({root.lod.center, root.lod.radius, level.maxError}) > thresholdPixels) {
      queue.push_back(level.rootNode);
    }
  }
  std::vector<uint32_t> cut;
  for (size_t q = 0; q < queue.size(); ++q) {
    const ClusterBvhNode &node = lod.nodes[queue[q]];
    if (project(node.lod) <= thresholdPixels) {
      continue;
    }
    for (uint32_t i = node.firstChild; i < node.firstChild + node.childCount; ++i) {
      if (!node.leaf) {
        queue.push_back(i);
      } else if (lodVisible(lod.clusters[i], camera, scale, projY, nearPlane, screenHeight,
                            thresholdPixels, orthoPixelsPerMeter)) {
        cut.push_back(i);
      }
    }
  }
  return cut;
}

} // namespace meridian
