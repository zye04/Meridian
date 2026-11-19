#include <doctest/doctest.h>
#include <meridian/mesh.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace meridian;

namespace {

// A bumpy 10 x 10 m grid of side x side quads, curved everywhere so
// simplification always has some error.
MeshData bumpyGrid(int side) {
  MeshData mesh;
  for (int z = 0; z <= side; ++z) {
    for (int x = 0; x <= side; ++x) {
      Vertex v;
      const float px = 10.0f * x / side - 5.0f;
      const float pz = 10.0f * z / side - 5.0f;
      v.position = {px, 0.3f * std::sin(px * 1.3f) * std::cos(pz * 1.7f) +
                            0.02f * (px * px + pz * pz),
                    pz};
      v.normal = {0.0f, 1.0f, 0.0f};
      v.uvX = px;
      v.uvY = pz;
      mesh.vertices.push_back(v);
    }
  }
  for (int z = 0; z < side; ++z) {
    for (int x = 0; x < side; ++x) {
      const uint32_t i = z * (side + 1) + x;
      const uint32_t j = i + side + 1;
      mesh.indices.insert(mesh.indices.end(), {i, j, i + 1, i + 1, j, j + 1});
    }
  }
  return mesh;
}

float triangleArea(const MeshData &mesh, uint32_t a, uint32_t b, uint32_t c) {
  const Vec3 pa = mesh.vertices[a].position;
  return 0.5f * length(cross(mesh.vertices[b].position - pa,
                             mesh.vertices[c].position - pa));
}

struct Cut {
  size_t triangles = 0;
  float area = 0.0f;
};

Cut selectCut(const MeshData &mesh, const ClusterLod &lod, Vec3 camera,
              float threshold) {
  Cut cut;
  for (const Cluster &c : lod.clusters) {
    if (!lodVisible(c, camera, 1.0f, 1.0f, 0.1f, 1080.0f, threshold)) {
      continue;
    }
    cut.triangles += c.triangleCount;
    for (uint32_t t = 0; t < c.triangleCount; ++t) {
      const uint8_t *local = &lod.triangles[(c.firstTriangle + t) * 3];
      const uint32_t *vertices = &lod.vertices[c.firstVertex];
      cut.area += triangleArea(mesh, vertices[local[0]], vertices[local[1]],
                               vertices[local[2]]);
    }
  }
  return cut;
}

} // namespace

TEST_CASE("a mesh smaller than a cluster is one original cluster") {
  const MeshData quad = bumpyGrid(1);
  const ClusterLod lod = buildClusterLod(quad);
  REQUIRE(lod.clusters.size() == 1);
  const Cluster &c = lod.clusters[0];
  CHECK(c.triangleCount == 2);
  CHECK(c.refined.error < 0.0f);
  CHECK(c.self.error == FLT_MAX);
  for (float threshold : {0.0f, 1.0f, 1000.0f}) {
    CHECK(lodVisible(c, {0, 1, 100}, 1.0f, 1.0f, 0.1f, 1080.0f, threshold));
  }
}

TEST_CASE("cluster LOD errors grow toward coarser clusters") {
  const MeshData mesh = bumpyGrid(100);
  const ClusterLod lod = buildClusterLod(mesh);
  CHECK(lod.clusters.size() > 157);
  for (const Cluster &c : lod.clusters) {
    CHECK(c.triangleCount <= 128);
    CHECK(c.vertexCount <= 128);
    if (c.refined.error >= 0.0f) {
      CHECK(c.refined.error <= c.self.error);
    }
  }
}

TEST_CASE("the selected cut covers the surface once") {
  const MeshData mesh = bumpyGrid(100);
  const ClusterLod lod = buildClusterLod(mesh);
  float area = 0.0f;
  for (size_t i = 0; i < mesh.indices.size(); i += 3) {
    area += triangleArea(mesh, mesh.indices[i], mesh.indices[i + 1],
                         mesh.indices[i + 2]);
  }
  const Vec3 camera{0.0f, 5.0f, 20.0f};
  CHECK(selectCut(mesh, lod, camera, 0.0f).triangles == mesh.indices.size() / 3);
  size_t previous = SIZE_MAX;
  for (float threshold : {0.0f, 1.0f, 8.0f, 64.0f}) {
    const Cut cut = selectCut(mesh, lod, camera, threshold);
    CHECK(cut.area == doctest::Approx(area).epsilon(0.05));
    CHECK(cut.triangles <= previous);
    previous = cut.triangles;
  }
  CHECK(previous < mesh.indices.size() / 3 / 4);
}

TEST_CASE("projected error falls with distance and grows with scale") {
  const LodBounds b{{0.0f, 0.0f, 0.0f}, 1.0f, 0.01f};
  const float near = projectedErrorPixels(b, {0, 0, 11}, 1.0f, 1.0f, 0.1f, 1000.0f);
  const float far = projectedErrorPixels(b, {0, 0, 21}, 1.0f, 1.0f, 0.1f, 1000.0f);
  CHECK(near == doctest::Approx(2.0f * far));
  CHECK(near == doctest::Approx(0.01f / 10.0f * 0.5f * 1000.0f));
  // Scale 2: error 0.02 at distance 11 - 2.
  CHECK(projectedErrorPixels(b, {0, 0, 11}, 2.0f, 1.0f, 0.1f, 1000.0f) ==
        doctest::Approx(0.02f / 9.0f * 0.5f * 1000.0f));
}

namespace {

std::filesystem::path cacheFile(const char *name) {
  return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST_CASE("cluster cache round-trips") {
  const std::vector<ClusterLod> lods = {buildClusterLod(bumpyGrid(20)),
                                        buildClusterLod(bumpyGrid(1))};
  const auto file = cacheFile("meridian_round_trip.clusters");
  writeClusterCache(file, 42, lods);
  const auto read = readClusterCache(file, 42);
  REQUIRE(read);
  REQUIRE(read->size() == 2);
  for (size_t i = 0; i < lods.size(); ++i) {
    CHECK((*read)[i].vertices == lods[i].vertices);
    CHECK((*read)[i].triangles == lods[i].triangles);
    REQUIRE((*read)[i].clusters.size() == lods[i].clusters.size());
    CHECK(std::memcmp((*read)[i].clusters.data(), lods[i].clusters.data(),
                      lods[i].clusters.size() * sizeof(Cluster)) == 0);
    REQUIRE((*read)[i].nodes.size() == lods[i].nodes.size());
    CHECK(std::memcmp((*read)[i].nodes.data(), lods[i].nodes.data(),
                      lods[i].nodes.size() * sizeof(ClusterBvhNode)) == 0);
    REQUIRE((*read)[i].levels.size() == lods[i].levels.size());
    CHECK(std::memcmp((*read)[i].levels.data(), lods[i].levels.data(),
                      lods[i].levels.size() * sizeof(ClusterLevelInfo)) == 0);
  }
  std::filesystem::remove(file);
}

TEST_CASE("cluster cache rejects another source key") {
  const auto file = cacheFile("meridian_key.clusters");
  writeClusterCache(file, 42, {buildClusterLod(bumpyGrid(4))});
  CHECK_FALSE(readClusterCache(file, 43));
  std::filesystem::remove(file);
}

TEST_CASE("truncated or missing cluster cache is rejected") {
  const auto file = cacheFile("meridian_truncated.clusters");
  writeClusterCache(file, 7, {buildClusterLod(bumpyGrid(20))});
  std::filesystem::resize_file(file, std::filesystem::file_size(file) / 2);
  CHECK_FALSE(readClusterCache(file, 7));
  std::filesystem::remove(file);
  CHECK_FALSE(readClusterCache(file, 7));
}

TEST_CASE("a cluster cache that can't be written throws and leaves nothing") {
  // A directory in the way makes the final rename fail.
  const auto file = cacheFile("meridian_blocked.clusters");
  std::filesystem::create_directory(file);
  CHECK_THROWS(writeClusterCache(file, 1, {buildClusterLod(bumpyGrid(4))}));
  CHECK_FALSE(std::filesystem::exists(file.string() + ".partial"));
  std::filesystem::remove(file);
}

TEST_CASE("damaged cluster hierarchies are invalid") {
  const MeshData mesh = bumpyGrid(20);
  const ClusterLod lod = buildClusterLod(mesh);
  CHECK(validClusterLod(lod, mesh.vertices.size()));
  CHECK_FALSE(validClusterLod(lod, mesh.vertices.size() / 2));

  ClusterLod local = lod;
  local.triangles[0] = 255;
  CHECK_FALSE(validClusterLod(local, mesh.vertices.size()));

  ClusterLod range = lod;
  range.clusters.back().firstTriangle = static_cast<uint32_t>(range.triangles.size());
  CHECK_FALSE(validClusterLod(range, mesh.vertices.size()));

  ClusterLod size = lod;
  size.clusters[0].vertexCount = 129;
  CHECK_FALSE(validClusterLod(size, mesh.vertices.size()));

  const MeshData bigMesh = bumpyGrid(100);
  const ClusterLod big = buildClusterLod(bigMesh);
  REQUIRE(validClusterLod(big, bigMesh.vertices.size()));

  ClusterLod leaf = big;
  leaf.nodes.back().firstChild = static_cast<uint32_t>(leaf.clusters.size());
  CHECK_FALSE(validClusterLod(leaf, bigMesh.vertices.size()));

  ClusterLod cycle = big;
  REQUIRE_FALSE(cycle.nodes[cycle.levels[0].rootNode].leaf);
  cycle.nodes[cycle.levels[0].rootNode].firstChild = cycle.levels[0].rootNode;
  CHECK_FALSE(validClusterLod(cycle, bigMesh.vertices.size()));

  ClusterLod noBvh = big;
  noBvh.nodes.clear();
  noBvh.levels.clear();
  CHECK_FALSE(validClusterLod(noBvh, bigMesh.vertices.size()));

  ClusterLod uncovered = big;
  --uncovered.nodes.back().childCount;
  CHECK_FALSE(validClusterLod(uncovered, bigMesh.vertices.size()));

  ClusterLod twice = big;
  REQUIRE((twice.nodes.back().leaf && twice.nodes[twice.nodes.size() - 2].leaf));
  twice.nodes.back().firstChild = twice.nodes[twice.nodes.size() - 2].firstChild;
  CHECK_FALSE(validClusterLod(twice, bigMesh.vertices.size()));

  ClusterLod root = big;
  root.levels.back().rootNode = static_cast<uint32_t>(root.nodes.size());
  CHECK_FALSE(validClusterLod(root, bigMesh.vertices.size()));
}

TEST_CASE("a cluster cache of the previous version is rejected") {
  const auto file = cacheFile("meridian_old_version.clusters");
  writeClusterCache(file, 5, {buildClusterLod(bumpyGrid(4))});
  CHECK(readClusterCache(file, 5));
  uint32_t version = 0;
  {
    std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
    io.seekg(4);
    io.read(reinterpret_cast<char *>(&version), sizeof(version));
    // The format before the BVH was stored.
    version = (version & 0xffffu) | (2u << 16);
    io.seekp(4);
    io.write(reinterpret_cast<const char *>(&version), sizeof(version));
  }
  CHECK_FALSE(readClusterCache(file, 5));
  std::filesystem::remove(file);
}

namespace {

// Stands in for island_tree_01: a solid trunk (simplifies normally) under a
// crown of disjoint quads (simplification gets stuck early, leaving terminal
// groups at several depths).
MeshData syntheticTree() {
  MeshData mesh;
  constexpr int kAround = 64;
  constexpr int kRings = 400;
  for (int r = 0; r <= kRings; ++r) {
    for (int a = 0; a <= kAround; ++a) {
      const float angle = 6.2831853f * a / kAround;
      const float y = 12.0f * r / kRings;
      const float radius = 0.5f + 0.05f * std::sin(y * 3.0f + angle * 2.0f);
      Vertex v;
      v.position = {radius * std::cos(angle), y, radius * std::sin(angle)};
      v.normal = {std::cos(angle), 0.0f, std::sin(angle)};
      v.uvX = static_cast<float>(a) / kAround;
      v.uvY = y;
      mesh.vertices.push_back(v);
    }
  }
  for (int r = 0; r < kRings; ++r) {
    for (int a = 0; a < kAround; ++a) {
      const uint32_t i = r * (kAround + 1) + a;
      const uint32_t j = i + kAround + 1;
      mesh.indices.insert(mesh.indices.end(), {i, i + 1, j, i + 1, j + 1, j});
    }
  }
  uint32_t state = 12345;
  const auto random = [&] {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 16777216.0f;
  };
  for (int q = 0; q < 750000; ++q) {
    const Vec3 center{random() * 10.0f - 5.0f, 8.0f + random() * 8.0f,
                      random() * 10.0f - 5.0f};
    const Vec3 u = normalize(Vec3{random() - 0.5f, random() - 0.5f, random() - 0.5f});
    const Vec3 v = normalize(cross(u, Vec3{0.3f, 1.0f, 0.2f})) * 0.1f;
    const Vec3 w = cross(u, v);
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const Vec3 corner : {center - v - w, center + v - w, center + v + w,
                              center - v + w}) {
      Vertex vertex;
      vertex.position = corner;
      vertex.normal = u;
      mesh.vertices.push_back(vertex);
    }
    mesh.indices.insert(mesh.indices.end(),
                        {base, base + 1, base + 2, base, base + 2, base + 3});
  }
  return mesh;
}

const ClusterLod &cachedTree() {
  static const ClusterLod lod = [] {
    const auto file = cacheFile("meridian_tree.clusters");
    writeClusterCache(file, 9, {buildClusterLod(syntheticTree())});
    auto read = readClusterCache(file, 9);
    std::filesystem::remove(file);
    REQUIRE(read);
    return std::move(read->front());
  }();
  return lod;
}

bool contains(Vec3 center, float radius, Vec3 innerCenter, float innerRadius) {
  return length(innerCenter - center) + innerRadius <= radius;
}

void checkBvh(const ClusterLod &lod) {
  std::vector<int> leafOf(lod.clusters.size(), 0);
  for (const ClusterBvhNode &node : lod.nodes) {
    REQUIRE(node.childCount > 0);
    for (uint32_t i = node.firstChild; i < node.firstChild + node.childCount; ++i) {
      if (node.leaf) {
        const Cluster &c = lod.clusters[i];
        ++leafOf[i];
        CHECK(node.lod.error >= c.self.error);
        CHECK(contains(node.lod.center, node.lod.radius, c.self.center, c.self.radius));
        if (c.refined.error >= 0.0f) {
          CHECK(contains(node.lod.center, node.lod.radius, c.refined.center,
                         c.refined.radius));
        }
        CHECK(contains(node.boundsCenter, node.boundsRadius, c.boundsCenter,
                       c.boundsRadius));
      } else {
        const ClusterBvhNode &child = lod.nodes[i];
        CHECK(node.lod.error >= child.lod.error);
        CHECK(contains(node.lod.center, node.lod.radius, child.lod.center,
                       child.lod.radius));
        CHECK(contains(node.boundsCenter, node.boundsRadius, child.boundsCenter,
                       child.boundsRadius));
      }
    }
  }
  CHECK(std::all_of(leafOf.begin(), leafOf.end(), [](int n) { return n == 1; }));
}

std::vector<uint32_t> flatCut(const ClusterLod &lod, Vec3 camera, float scale,
                              float projY, float threshold) {
  std::vector<uint32_t> cut;
  for (uint32_t i = 0; i < lod.clusters.size(); ++i) {
    if (lodVisible(lod.clusters[i], camera, scale, projY, 0.1f, 1440.0f, threshold)) {
      cut.push_back(i);
    }
  }
  return cut;
}

void checkTraversal(const ClusterLod &lod, Vec3 center) {
  for (const Vec3 direction : {Vec3{0, 0, 1}, normalize(Vec3{1, 0.5f, -0.3f}),
                               normalize(Vec3{-0.2f, -1, 0.4f})}) {
    for (float distance : {1.0f, 10.0f, 100.0f, 1000.0f}) {
      for (float threshold : {0.5f, 1.0f, 4.0f}) {
        for (float scale : {1.0f, 2.5f}) {
          for (float projY : {1.2f, 3.0f}) {
            const Vec3 camera = center + direction * distance;
            std::vector<uint32_t> bvh = selectClustersBvh(lod, camera, scale, projY,
                                                          0.1f, 1440.0f, threshold);
            std::sort(bvh.begin(), bvh.end());
            const std::vector<uint32_t> flat =
                flatCut(lod, camera, scale, projY, threshold);
            CHECK(!flat.empty());
            CHECK(bvh == flat);
          }
        }
      }
    }
  }
}

} // namespace

TEST_CASE("every cluster BVH node bounds what is below it") {
  const ClusterLod &tree = cachedTree();
  CHECK(tree.levels.size() >= 4);
  checkBvh(tree);
  checkBvh(buildClusterLod(bumpyGrid(1)));
  checkBvh(buildClusterLod(bumpyGrid(100)));
}

TEST_CASE("the BVH traversal selects exactly the flat cut") {
  const ClusterLod &tree = cachedTree();
  checkTraversal(tree, {0.0f, 8.0f, 0.0f});
  const ClusterLod single = buildClusterLod(bumpyGrid(1));
  REQUIRE(single.clusters.size() == 1);
  checkTraversal(single, {});
  checkTraversal(buildClusterLod(bumpyGrid(100)), {});
}
