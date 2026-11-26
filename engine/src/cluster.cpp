#include <meridian/cluster.h>
#include <meridian/farfield.h>
#include <meridian/mesh.h>

#include <meshoptimizer.h>
#include <clusterlod.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <map>
#include <numeric>
#include <bit>
#include <cfloat>
#include <cstdio>
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
constexpr uint32_t kCacheVersion = (8u << 16) | MESHOPTIMIZER_VERSION;
// Most a leaf piece is scaled up (linearly) to keep its group's area.
constexpr float kMaxLeafScale = 4.0f;
constexpr size_t kBvhBranching = 8;
// Bump when voxelize's output or the voxel levels' rules change for the same
// inputs.
constexpr uint32_t kVoxelizerVersion = 11;
// A voxel cluster holds the bricks of one aligned region this many voxels
// on a side (8^3 bricks at most): fine levels of sparse foliage still fill
// clusters of tens of bricks (4^3-brick regions averaged 6-11 bricks and
// overflowed the voxel list at the forest overview).
constexpr int64_t kRegionVoxels = 32;
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

// The voxel levels of one mesh's DAG, built through clodBuild's voxel hooks
// (third_party/clusterlod): decide voxelizes a group's original triangles,
// attach emits the octree of voxel levels above the group it replaced.
struct VoxelLevels {
  const MeshData &mesh;
  const VoxelSource &source;
  ClusterLod &lod;
  std::vector<LodBounds> &groups;
  std::vector<GroupRange> &groupRanges;
  std::vector<int> &refinedGroups;
  std::vector<int> &clusterGroups;
  // No voxel finer than the model's height / 512: it sets where voxels take
  // over (the first level whose error reaches it), ~117 m for a 25 m beech
  // at 1440p, where its triangles are sub-pixel and many.
  float minEdge = 1e-4f;

  static float edgeFor(void *context, float minError, float maxError) {
    return static_cast<VoxelLevels *>(context)->edgeFor(minError, maxError);
  }
  static void emit(void *context, const unsigned int *indices, size_t indexCount, float edge,
                   int group, clodBounds, int depth) {
    static_cast<VoxelLevels *>(context)->emit(indices, indexCount, edge, group, depth);
  }

  // Voxels take over once they are no worse than the triangles (Nanite
  // Foliage's rule): the coarsest lattice edge at or under the triangles'
  // error, not finer than the mesh's minimum edge (0: keep the triangles).
  // Not a primitive count contest: only the voxels a view sees draw
  // (coverage, hidden voxels skipped), every triangle does. The group's
  // error is at least its children's (clodBuild; monotonic, the next
  // level's twice the edge is over it again). Stuck groups (maxError
  // FLT_MAX) would draw forever: voxels at once.
  float edgeFor(float minError, float maxError) const {
    const float floorEdge = std::exp2(std::floor(std::log2(minEdge)));
    const float edge =
        maxError == FLT_MAX
            ? std::max(floorEdge, std::exp2(std::ceil(std::log2(std::max(minError, minEdge)))))
            : std::max(floorEdge, std::exp2(std::floor(std::log2(maxError))));
    return edge <= maxError ? edge : 0.0f;
  }

  // A group that went to voxels (clodBuild pools a pass's groups by cell):
  // its triangles voxelized, split by region, wait for the shared hierarchy
  // (finish).
  std::vector<VoxelTriangle> triangles(const unsigned int *indices, size_t indexCount) const {
    std::vector<VoxelTriangle> out(indexCount / 3);
    for (size_t t = 0; t < out.size(); ++t) {
      VoxelTriangle &v = out[t];
      for (int k = 0; k < 3; ++k) {
        const Vertex &vertex = mesh.vertices[indices[t * 3 + k]];
        v.p[k] = vertex.position;
        v.n[k] = vertex.normal;
        v.uv[k][0] = vertex.uvX;
        v.uv[k][1] = vertex.uvY;
      }
      v.material = source.material;
    }
    return out;
  }

  void emit(const unsigned int *indices, size_t indexCount, float edge, int group, int depth) {
    const VoxelGrid grid = voxelize(triangles(indices, indexCount), source.materials, edge);
    for (const Region &r : regions(grid)) {
      VoxelGrid part;
      part.edge = edge;
      for (uint32_t b : r.bricks) {
        part.bricks.push_back(grid.bricks[b]);
      }
      levels[edge].push_back({std::move(part), group, r.key});
    }
    firstDepth = std::max(firstDepth, depth + 1);
  }

  struct Region {
    std::array<int64_t, 3> key;
    std::vector<uint32_t> bricks; // into the level's grid
  };

  static int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

  // A level's voxel clusters: the bricks of each aligned kRegionVoxels^3
  // region.
  static std::vector<Region> regions(const VoxelGrid &grid) {
    std::map<std::array<int64_t, 3>, Region> byKey;
    for (uint32_t b = 0; b < grid.bricks.size(); ++b) {
      const VoxelBrick &brick = grid.bricks[b];
      const std::array<int64_t, 3> key = {floorDiv(brick.origin[0], kRegionVoxels),
                                          floorDiv(brick.origin[1], kRegionVoxels),
                                          floorDiv(brick.origin[2], kRegionVoxels)};
      Region &r = byKey[key];
      r.key = key;
      r.bricks.push_back(b);
    }
    std::vector<Region> out;
    for (auto &[key, r] : byKey) {
      out.push_back(std::move(r));
    }
    return out;
  }

  // One voxel hierarchy for the whole mesh, edge by edge from the finest:
  // the clusters of each coarser region form a group, replaced by one
  // cluster of their voxels combined (whichever group each came from) and
  // merged to the next edge, which joins the groups that went to voxels at
  // that edge. Down to a handful of clusters, as a far tree draws.
  void finish() {
    int depth = firstDepth;
    while (!levels.empty()) {
      auto level = levels.begin();
      const float edge = level->first;
      std::vector<Node> nodes = std::move(level->second);
      levels.erase(level);
      size_t voxels = 0;
      for (const Node &n : nodes) {
        for (const VoxelBrick &b : n.grid.bricks) {
          voxels += std::popcount(b.occupied);
        }
      }
      // The lattice is anchored at the origin, so regions on either side of
      // it never merge by halving their keys: the last few clusters merge
      // into one, so a far mesh draws a single cluster.
      const bool last = levels.empty() && voxels <= 64;
      if (last && nodes.size() == 1) {
        std::vector<Sphere> spheres;
        std::vector<Cluster> clusters;
        for (const Node &n : nodes) {
          clusters.push_back(regionCluster(n.grid, n.refined));
          spheres.push_back({clusters.back().boundsCenter, clusters.back().boundsRadius});
        }
        const Sphere s = enclose(spheres);
        emitGroup(clusters, {s.center, s.radius, FLT_MAX}, depth);
        return;
      }
      std::map<std::array<int64_t, 3>, std::vector<uint32_t>> parents;
      for (uint32_t i = 0; i < nodes.size(); ++i) {
        const std::array<int64_t, 3> parent = {floorDiv(nodes[i].key[0], 2),
                                               floorDiv(nodes[i].key[1], 2),
                                               floorDiv(nodes[i].key[2], 2)};
        parents[last ? std::array<int64_t, 3>{} : parent].push_back(i);
      }
      for (const auto &[key, members] : parents) {
        std::vector<Cluster> clusters;
        std::vector<Sphere> spheres;
        std::vector<const VoxelGrid *> grids;
        // At least twice the error of every group the members replace: a
        // voxel cluster made from triangles gives way at kVoxelPixels times
        // the threshold, its triangles at the threshold, so the cut covers
        // once only if this projects at least twice theirs (the bounds
        // enclose theirs, so twice the error suffices).
        float error = edge * 2.0f;
        for (uint32_t i : members) {
          clusters.push_back(regionCluster(nodes[i].grid, nodes[i].refined));
          spheres.push_back({clusters.back().boundsCenter, clusters.back().boundsRadius});
          const LodBounds &refined = groups[nodes[i].refined];
          spheres.push_back({refined.center, refined.radius});
          error = std::max(error, refined.error * kVoxelPixels);
          grids.push_back(&nodes[i].grid);
        }
        // The group's bounds enclose its members' and their own groups', so
        // projected errors stay monotonic along every path.
        const Sphere s = enclose(spheres);
        const int id = emitGroup(clusters, {s.center, s.radius, error}, depth);
        levels[edge * 2.0f].push_back({mergeVoxelGrid(combineVoxelGrids(grids)), id, key});
      }
      ++depth;
    }
  }

  // A cluster of a region's bricks; its group (self) is set by emitGroup.
  Cluster regionCluster(const VoxelGrid &grid, int refined) {
    Cluster c;
    c.firstBrick = uint32_t(lod.bricks.size());
    c.brickCount = uint32_t(grid.bricks.size());
    c.voxelEdge = grid.edge;
    c.refinedGroup = refined;
    Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const VoxelBrick &brick : grid.bricks) {
      VoxelBrickRef ref;
      for (int k = 0; k < 3; ++k) {
        ref.origin[k] = brick.origin[k];
      }
      ref.firstVoxel = uint32_t(lod.voxels.size());
      ref.occupied = brick.occupied;
      lod.bricks.push_back(ref);
      for (uint32_t i = 0; i < 64; ++i) {
        if (!((brick.occupied >> i) & 1u)) {
          continue;
        }
        lod.voxels.push_back(packVoxelPayload(brick.voxels[i]));
        const Vec3 v0{(brick.origin[0] + float(i % 4)) * grid.edge,
                      (brick.origin[1] + float(i / 4 % 4)) * grid.edge,
                      (brick.origin[2] + float(i / 16)) * grid.edge};
        const Vec3 v1 = v0 + Vec3{grid.edge, grid.edge, grid.edge};
        lo = {std::min(lo.x, v0.x), std::min(lo.y, v0.y), std::min(lo.z, v0.z)};
        hi = {std::max(hi.x, v1.x), std::max(hi.y, v1.y), std::max(hi.z, v1.z)};
      }
    }
    c.boundsCenter = (lo + hi) * 0.5f;
    c.boundsRadius = length(hi - lo) * 0.5f * (1.0f + 1e-4f);
    return c;
  }

  // Emits clusters as one group (contiguous, as GroupRange wants).
  int emitGroup(const std::vector<Cluster> &clusters, LodBounds bounds, int depth) {
    const int id = int(groups.size());
    groups.push_back(bounds);
    groupRanges.push_back({depth, uint32_t(lod.clusters.size()), uint32_t(clusters.size())});
    for (Cluster c : clusters) {
      c.self = bounds;
      c.group = uint32_t(id);
      lod.clusters.push_back(c);
      refinedGroups.push_back(c.refinedGroup);
      clusterGroups.push_back(id);
    }
    return id;
  }

  struct Node {
    VoxelGrid grid; // one region's bricks
    int refined;
    std::array<int64_t, 3> key;
  };
  std::map<float, std::vector<Node>> levels;
  int firstDepth = 0;
};

} // namespace

ClusterLod buildClusterLod(const MeshData &mesh, const VoxelSource *voxels) {
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
  clodConfig config = clodDefaultConfig(kMaxClusterTriangles);
  VoxelLevels voxelLevels{mesh, voxels ? *voxels : VoxelSource{}, lod, groups,
                          groupRanges, refinedGroups, clusterGroups};
  if (voxels && mesh.voxelize) {
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (const Vertex &v : mesh.vertices) {
      lo = std::min(lo, v.position.y);
      hi = std::max(hi, v.position.y);
    }
    voxelLevels.minEdge = std::max(std::max(hi - lo, mesh.voxelModelHeight) / 512.0f, 1e-4f);
    config.voxel_context = &voxelLevels;
    config.voxel_edge = &VoxelLevels::edgeFor;
    config.voxel_emit = &VoxelLevels::emit;
    config.voxel_cell = float(kRegionVoxels);
  }
  clodBuild(config, input,
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
                cluster.group = static_cast<uint32_t>(groupIndex);
                cluster.refinedGroup = c.refined;
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
  voxelLevels.finish();
  if (voxels && mesh.voxelize) {
    packVoxelBricks(voxelize(voxelLevels.triangles(mesh.indices.data(), mesh.indices.size()),
                             voxels->materials, kFarTemplateVoxel),
                    lod.farBricks, lod.farVoxels);
  }

  for (size_t i = 0; i < lod.clusters.size(); ++i) {
    const int refined = refinedGroups[i];
    lod.clusters[i].refined =
        refined < 0 ? LodBounds{{}, 0.0f, -1.0f} : groups[refined];
  }
  // Voxels keep a voxelizing mesh's leaves from thinning; scaling its
  // simplified leaves up (to 16x area) only overdraws (the forest edge's
  // early scene pass 188 ms against 6.5 ms without).
  if (mesh.leaf && !(voxels && mesh.voxelize)) {
    // Voxel clusters have no area to keep: the leaf move applies to triangle
    // replacements only.
    std::vector<int> triangleRefined = refinedGroups;
    for (size_t i = 0; i < lod.clusters.size(); ++i) {
      if (lod.clusters[i].brickCount > 0) {
        triangleRefined[i] = -1;
      }
    }
    keepLeafArea(mesh, lod, clusterGroups, triangleRefined, groups);
    // The move raised groups' errors and radii; every group above follows,
    // so no member's own error projects larger than its group's (groups
    // come after the groups their members were made from).
    std::vector<std::vector<size_t>> members(groups.size());
    for (size_t i = 0; i < lod.clusters.size(); ++i) {
      members[clusterGroups[i]].push_back(i);
    }
    for (size_t g = 0; g < groups.size(); ++g) {
      for (size_t i : members[g]) {
        if (refinedGroups[i] < 0) {
          continue;
        }
        const LodBounds &refined = groups[refinedGroups[i]];
        if (groups[g].error != FLT_MAX) {
          groups[g].error = std::max(groups[g].error, refined.error);
        }
        groups[g].radius = std::max(groups[g].radius,
                                    length(groups[g].center - refined.center) + refined.radius);
      }
    }
    for (size_t i = 0; i < lod.clusters.size(); ++i) {
      lod.clusters[i].self = groups[clusterGroups[i]];
      if (refinedGroups[i] >= 0) {
        lod.clusters[i].refined = groups[refinedGroups[i]];
      }
    }
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
      writeArray(out, lod.bricks);
      writeArray(out, lod.voxels);
      writeArray(out, lod.voxelAxis);
      writeArray(out, lod.farBricks);
      writeArray(out, lod.farVoxels);
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

uint32_t clusterCacheVersion() { return kCacheVersion; }

uint32_t voxelizerVersion() { return kVoxelizerVersion; }

std::string clusterCacheExtension() {
  char extension[32];
  std::snprintf(extension, sizeof(extension), ".v%08x.%u.clusters", kCacheVersion,
                kVoxelizerVersion);
  return extension;
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
        !readArray(in, size, lod.addedPositions) || !readArray(in, size, lod.bricks) ||
        !readArray(in, size, lod.voxels) || !readArray(in, size, lod.voxelAxis) ||
        !readArray(in, size, lod.farBricks) || !readArray(in, size, lod.farVoxels)) {
      return std::nullopt;
    }
  }
  return lods;
}

bool validClusterLod(const ClusterLod &lod, size_t meshVertexCount) {
  if (!lod.voxelAxis.empty() && lod.voxelAxis.size() != lod.voxels.size()) {
    return false;
  }
  for (const VoxelBrickRef &brick : lod.bricks) {
    if (uint64_t{brick.firstVoxel} + std::popcount(brick.occupied) > lod.voxels.size()) {
      return false;
    }
  }
  for (const VoxelBrickRef &brick : lod.farBricks) {
    if (uint64_t{brick.firstVoxel} + std::popcount(brick.occupied) > lod.farVoxels.size()) {
      return false;
    }
  }
  for (const Cluster &c : lod.clusters) {
    if (uint64_t{c.firstBrick} + c.brickCount > lod.bricks.size() ||
        (c.brickCount > 0 && (c.triangleCount > 0 || !(c.voxelEdge > 0.0f)))) {
      return false;
    }
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

void bakeModelAxisDepths(std::vector<ClusterLod> &meshes) {
  // Per edge: every voxel of the model, by lattice position, summed where
  // meshes overlap.
  struct Cell {
    float depth[3] = {};
  };
  struct Ref {
    size_t mesh;
    uint32_t voxel;
    int64_t c[3];
  };
  std::map<float, std::map<std::array<int64_t, 3>, Cell>> cells;
  std::map<float, std::vector<Ref>> refs;
  for (size_t m = 0; m < meshes.size(); ++m) {
    ClusterLod &lod = meshes[m];
    lod.voxelAxis.assign(lod.voxels.size(), 0);
    for (const Cluster &c : lod.clusters) {
      for (uint32_t b = c.firstBrick; b < c.firstBrick + c.brickCount; ++b) {
        const VoxelBrickRef &brick = lod.bricks[b];
        uint32_t voxel = brick.firstVoxel;
        for (uint32_t i = 0; i < 64; ++i) {
          if (!((brick.occupied >> i) & 1u)) {
            continue;
          }
          const std::array<int64_t, 3> key = {brick.origin[0] + int64_t(i % 4),
                                              brick.origin[1] + int64_t(i / 4 % 4),
                                              brick.origin[2] + int64_t(i / 16)};
          const VoxelPayload v = unpackVoxelPayload(lod.voxels[voxel]);
          Cell &cell = cells[c.voxelEdge][key];
          for (int a = 0; a < 3; ++a) {
            cell.depth[a] += v.depth[a];
          }
          refs[c.voxelEdge].push_back({m, voxel, {key[0], key[1], key[2]}});
          ++voxel;
        }
      }
    }
  }
  for (auto &[edge, grid] : cells) {
    // Per axis and line of cells along it: positions and depths in order,
    // with prefix sums, so each voxel's depth beyond it is a lookup.
    std::array<std::map<std::array<int64_t, 2>, std::vector<std::pair<int64_t, float>>>, 3> lines;
    for (const auto &[key, cell] : grid) {
      for (int a = 0; a < 3; ++a) {
        lines[a][{key[(a + 1) % 3], key[(a + 2) % 3]}].push_back({key[a], cell.depth[a]});
      }
    }
    std::array<std::map<std::array<int64_t, 2>, std::vector<float>>, 3> prefix;
    for (int a = 0; a < 3; ++a) {
      for (auto &[line, cellsOnLine] : lines[a]) {
        std::sort(cellsOnLine.begin(), cellsOnLine.end());
        std::vector<float> &sum = prefix[a][line];
        sum.assign(cellsOnLine.size() + 1, 0.0f);
        for (size_t i = 0; i < cellsOnLine.size(); ++i) {
          sum[i + 1] = sum[i] + cellsOnLine[i].second;
        }
      }
    }
    for (const Ref &r : refs[edge]) {
      uint32_t word = 0;
      for (int a = 0; a < 3; ++a) {
        const std::array<int64_t, 2> line = {r.c[(a + 1) % 3], r.c[(a + 2) % 3]};
        const auto &cellsOnLine = lines[a][line];
        const std::vector<float> &sum = prefix[a][line];
        const size_t i = size_t(std::lower_bound(cellsOnLine.begin(), cellsOnLine.end(),
                                                 std::pair<int64_t, float>{r.c[a], -FLT_MAX}) -
                                cellsOnLine.begin());
        const float own = 0.5f * cellsOnLine[i].second;
        const float toward[2] = {own + sum.back() - sum[i + 1], own + sum[i]}; // +axis, -axis
        for (int s = 0; s < 2; ++s) {
          const uint32_t q = std::min<uint32_t>(15, uint32_t(std::lround(toward[s] / kVoxelAxisDepthStep)));
          word |= q << (4 * (2 * a + s));
        }
      }
      meshes[r.mesh].voxelAxis[r.voxel] = word;
    }
  }
  // Drop what no direction sees.
  const uint32_t hidden = uint32_t(std::ceil(kVoxelHiddenDepth / kVoxelAxisDepthStep));
  const auto visible = [&](uint32_t word) {
    for (int k = 0; k < 6; ++k) {
      if (((word >> (4 * k)) & 15u) <= hidden) {
        return true;
      }
    }
    return false;
  };
  for (ClusterLod &lod : meshes) {
    std::vector<VoxelBrickRef> bricks;
    std::vector<PackedVoxel> voxels;
    std::vector<uint32_t> axis;
    for (Cluster &c : lod.clusters) {
      if (c.brickCount == 0) {
        continue;
      }
      const uint32_t firstBrick = uint32_t(bricks.size());
      for (uint32_t b = c.firstBrick; b < c.firstBrick + c.brickCount; ++b) {
        const VoxelBrickRef &brick = lod.bricks[b];
        VoxelBrickRef kept = brick;
        kept.firstVoxel = uint32_t(voxels.size());
        kept.occupied = 0;
        uint32_t voxel = brick.firstVoxel;
        for (uint32_t i = 0; i < 64; ++i) {
          if (!((brick.occupied >> i) & 1u)) {
            continue;
          }
          if (visible(lod.voxelAxis[voxel])) {
            kept.occupied |= uint64_t{1} << i;
            voxels.push_back(lod.voxels[voxel]);
            axis.push_back(lod.voxelAxis[voxel]);
          }
          ++voxel;
        }
        if (kept.occupied != 0) {
          bricks.push_back(kept);
        }
      }
      // A cluster keeps at least one brick: none of them visible would mean
      // a solid block, never the case for foliage, but a voxel cluster must
      // stay one.
      if (bricks.size() == firstBrick) {
        VoxelBrickRef brick = lod.bricks[c.firstBrick];
        const uint32_t i = uint32_t(std::countr_zero(brick.occupied));
        brick.occupied = uint64_t{1} << i;
        const uint32_t voxel = lod.bricks[c.firstBrick].firstVoxel;
        brick.firstVoxel = uint32_t(voxels.size());
        voxels.push_back(lod.voxels[voxel]);
        axis.push_back(lod.voxelAxis[voxel]);
        bricks.push_back(brick);
      }
      c.firstBrick = firstBrick;
      c.brickCount = uint32_t(bricks.size()) - firstBrick;
    }
    lod.bricks = std::move(bricks);
    lod.voxels = std::move(voxels);
    lod.voxelAxis = std::move(axis);
  }
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

std::vector<LodThresholds> clusterThresholds(const ClusterLod &lod, float threshold) {
  std::vector<uint8_t> triangleGroup;
  for (const Cluster &c : lod.clusters) {
    if (c.brickCount == 0) {
      triangleGroup.resize(std::max<size_t>(triangleGroup.size(), size_t(c.group) + 1), 0);
      triangleGroup[c.group] = 1;
    }
  }
  std::vector<LodThresholds> thresholds;
  thresholds.reserve(lod.clusters.size());
  for (const Cluster &c : lod.clusters) {
    if (c.brickCount == 0) {
      thresholds.push_back({threshold, threshold});
      continue;
    }
    const bool fromTriangles = c.refinedGroup >= 0 &&
                               size_t(c.refinedGroup) < triangleGroup.size() &&
                               triangleGroup[size_t(c.refinedGroup)];
    thresholds.push_back(
        {threshold * kVoxelPixels, fromTriangles ? threshold : threshold * kVoxelPixels});
  }
  return thresholds;
}

bool lodVisible(const Cluster &cluster, Vec3 camera, float scale, float projY,
                float nearPlane, float screenHeight, LodThresholds thresholds,
                float orthoPixelsPerMeter) {
  const auto project = [&](const LodBounds &b) {
    return projectedErrorPixels(b, camera, scale, projY, nearPlane, screenHeight,
                                orthoPixelsPerMeter);
  };
  return project(cluster.self) > thresholds.self &&
         (cluster.refined.error < 0.0f || project(cluster.refined) <= thresholds.refined);
}

std::vector<uint32_t> selectClustersBvh(const ClusterLod &lod, Vec3 camera,
                                        float scale, float projY,
                                        float nearPlane, float screenHeight,
                                        float thresholdPixels,
                                        float orthoPixelsPerMeter, bool voxels) {
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
    // Voxel levels switch at up to kVoxelPixels times the threshold.
    if (finest <= thresholdPixels * kVoxelPixels &&
        (!voxels ||
         project({root.lod.center, root.lod.radius, level.maxError}) > thresholdPixels)) {
      queue.push_back(level.rootNode);
    }
  }
  std::vector<LodThresholds> thresholds = clusterThresholds(lod, thresholdPixels);
  if (!voxels) {
    // The upload's voxelEdge -1: triangle groups voxel clusters replace.
    std::vector<uint8_t> voxelGroup;
    for (const Cluster &c : lod.clusters) {
      if (c.brickCount > 0 && c.refinedGroup >= 0) {
        voxelGroup.resize(std::max<size_t>(voxelGroup.size(), size_t(c.refinedGroup) + 1), 0);
        voxelGroup[size_t(c.refinedGroup)] = 1;
      }
    }
    for (size_t i = 0; i < lod.clusters.size(); ++i) {
      const Cluster &c = lod.clusters[i];
      if (c.brickCount == 0 && c.group < voxelGroup.size() && voxelGroup[c.group]) {
        thresholds[i].self = -1.0f;
      }
    }
  }
  std::vector<uint32_t> cut;
  for (size_t q = 0; q < queue.size(); ++q) {
    const ClusterBvhNode &node = lod.nodes[queue[q]];
    if (voxels && project(node.lod) <= thresholdPixels) {
      continue;
    }
    for (uint32_t i = node.firstChild; i < node.firstChild + node.childCount; ++i) {
      if (!node.leaf) {
        queue.push_back(i);
      } else if ((voxels || lod.clusters[i].brickCount == 0) &&
                 lodVisible(lod.clusters[i], camera, scale, projY, nearPlane, screenHeight,
                            thresholds[i], orthoPixelsPerMeter)) {
        cut.push_back(i);
      }
    }
  }
  return cut;
}

bool voxelsAt(Vec3 instanceOrigin, Vec3 camera, float voxelNear) {
  return length(instanceOrigin - camera) > voxelNear;
}

bool voxelizesModel(std::span<const MeshData> meshes, std::span<const uint8_t> foliage) {
  constexpr float kCrownHeight = 4.0f; // mesh.mesh's crown rule
  for (size_t i = 0; i < meshes.size(); ++i) {
    if (!foliage[i] || meshes[i].vertices.empty()) {
      continue;
    }
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (const Vertex &v : meshes[i].vertices) {
      lo = std::min(lo, v.position.y);
      hi = std::max(hi, v.position.y);
    }
    if (hi - lo > kCrownHeight) {
      return true;
    }
  }
  return false;
}

uint64_t clusterSourceKey(std::span<const MeshData> meshes,
                          std::span<const std::vector<uint8_t>> voxelTextures,
                          std::span<const VoxelMaterial> voxelMaterials,
                          std::span<const uint32_t> primitiveMaterial,
                          std::span<const std::array<int, 3>> materialTextures) {
  // FNV-1a: the cache stays valid exactly as long as what it was built from.
  uint64_t hash = 14695981039346656037ull;
  const auto mix = [&](const void *data, size_t bytes) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < bytes; ++i) {
      hash = (hash ^ p[i]) * 1099511628211ull;
    }
  };
  for (const MeshData &mesh : meshes) {
    const uint64_t counts[] = {mesh.vertices.size(), mesh.indices.size(), mesh.leaf,
                               mesh.voxelize};
    mix(counts, sizeof(counts));
    mix(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
    mix(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
  }
  for (const std::vector<uint8_t> &texture : voxelTextures) {
    const uint64_t size = texture.size();
    mix(&size, sizeof(size));
    mix(texture.data(), texture.size());
  }
  // Field by field: the image pointers are per load and the struct has padding.
  const uint64_t materialCounts[] = {voxelMaterials.size(), primitiveMaterial.size(),
                                     materialTextures.size()};
  mix(materialCounts, sizeof(materialCounts));
  for (const VoxelMaterial &m : voxelMaterials) {
    const float factors[] = {m.baseColorFactor.x,   m.baseColorFactor.y,   m.baseColorFactor.z,
                             float(m.alphaMask),    m.alphaCutoff,         m.diffuseTransmission,
                             m.transmissionColor.x, m.transmissionColor.y, m.transmissionColor.z,
                             m.roughnessFactor};
    mix(factors, sizeof(factors));
  }
  mix(primitiveMaterial.data(), primitiveMaterial.size_bytes());
  mix(materialTextures.data(), materialTextures.size_bytes());
  mix(&kVoxelizerVersion, sizeof(kVoxelizerVersion));
  return hash;
}

} // namespace meridian
