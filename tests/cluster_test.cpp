#include <doctest/doctest.h>
#include <meridian/mesh.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

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
    if (!lodVisible(c, camera, 1.0f, 1.0f, 0.1f, 1080.0f, {threshold, threshold})) {
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
    CHECK(lodVisible(c, {0, 1, 100}, 1.0f, 1.0f, 0.1f, 1080.0f, {threshold, threshold}));
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
  const std::vector<LodThresholds> thresholds = clusterThresholds(lod, threshold);
  std::vector<uint32_t> cut;
  for (uint32_t i = 0; i < lod.clusters.size(); ++i) {
    if (lodVisible(lod.clusters[i], camera, scale, projY, 0.1f, 1440.0f, thresholds[i])) {
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

TEST_CASE("selectClustersBvh orthographic equals lodVisible") {
  const auto check = [](const ClusterLod &lod, Vec3 camera) {
    for (float pixelsPerMeter : {2048.0f / 50.0f, 2048.0f / 12800.0f}) {
      for (float threshold : {0.5f, 1.0f, 4.0f}) {
        for (float scale : {1.0f, 2.5f}) {
          std::vector<uint32_t> bvh = selectClustersBvh(lod, camera, scale, 1.0f, 0.1f, 1.0f,
                                                        threshold, pixelsPerMeter);
          std::sort(bvh.begin(), bvh.end());
          const std::vector<LodThresholds> thresholds = clusterThresholds(lod, threshold);
          std::vector<uint32_t> flat;
          for (uint32_t i = 0; i < lod.clusters.size(); ++i) {
            if (lodVisible(lod.clusters[i], camera, scale, 1.0f, 0.1f, 1.0f, thresholds[i],
                           pixelsPerMeter)) {
              flat.push_back(i);
            }
          }
          CHECK(!flat.empty());
          CHECK(bvh == flat);
        }
      }
    }
  };
  check(cachedTree(), {0.0f, 8.0f, 50.0f});
  check(buildClusterLod(bumpyGrid(1)), {});
  check(buildClusterLod(bumpyGrid(100)), {0.0f, 5.0f, 500.0f});
}

namespace {

// 3000 disjoint leaves in a 2 m crown: flat discs of 3 cm radius, 24 outline
// vertices around a centre. Simplification shrinks and drops such pieces.
MeshData leafCrown() {
  MeshData mesh;
  uint32_t state = 777;
  const auto random = [&] {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 16777216.0f;
  };
  for (int leaf = 0; leaf < 3000; ++leaf) {
    const Vec3 center{random() * 2.0f - 1.0f, random() * 2.0f, random() * 2.0f - 1.0f};
    const Vec3 n = normalize(Vec3{random() - 0.5f, random() - 0.5f, random() - 0.5f});
    const Vec3 u = normalize(cross(n, Vec3{0.3f, 1.0f, 0.2f}));
    const Vec3 v = cross(n, u);
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    Vertex middle;
    middle.position = center;
    middle.normal = n;
    mesh.vertices.push_back(middle);
    for (int k = 0; k < 24; ++k) {
      const float angle = 6.2831853f * k / 24;
      Vertex rim;
      rim.position = center + (u * std::cos(angle) + v * std::sin(angle)) * 0.03f;
      rim.normal = n;
      mesh.vertices.push_back(rim);
      mesh.indices.insert(mesh.indices.end(),
                          {base, base + 1 + k, base + 1 + (k + 1) % 24});
    }
  }
  return mesh;
}

float fullArea(const MeshData &mesh) {
  float area = 0.0f;
  for (size_t i = 0; i < mesh.indices.size(); i += 3) {
    area += triangleArea(mesh, mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]);
  }
  return area;
}

} // namespace

TEST_CASE("leaf meshes keep their area down the cluster LOD") {
  MeshData plain = leafCrown();
  plain.clusters = buildClusterLod(plain);
  CHECK(plain.clusters.addedPositions.empty());

  MeshData leaves = leafCrown();
  leaves.leaf = true;
  leaves.clusters = buildClusterLod(leaves);
  const size_t sourceVertices = leaves.vertices.size();
  CHECK_FALSE(leaves.clusters.addedPositions.empty());
  appendClusterVertices(leaves);
  CHECK(leaves.vertices.size() == sourceVertices + leaves.clusters.addedPositions.size());
  CHECK(validClusterLod(leaves.clusters, sourceVertices));

  const float full = fullArea(plain);
  // Farther, these isolated discs collapse at once into a sloppy level past the
  // scale limit; real crowns (connected sprigs) are gated by meridian-cook
  // --coverage up to the voxel switch.
  for (const float distance : {12.0f, 15.0f}) {
    CAPTURE(distance);
    const Vec3 camera{0.0f, 1.0f, distance};
    const Cut lost = selectCut(plain, plain.clusters, camera, 1.0f);
    const Cut kept = selectCut(leaves, leaves.clusters, camera, 1.0f);
    // Without compensation the coarse cut visibly thins the crown.
    CHECK(lost.area < 0.9f * full);
    CHECK(kept.area == doctest::Approx(full).epsilon(0.1));
    // The scaling's move counts as LOD error, so the cut may stay finer.
    CHECK(kept.triangles >= lost.triangles);
  }
}

TEST_CASE("added leaf vertices survive the cluster cache") {
  MeshData leaves = leafCrown();
  leaves.leaf = true;
  const ClusterLod lod = buildClusterLod(leaves);
  const auto file = cacheFile("meridian_leaves.clusters");
  writeClusterCache(file, 11, {lod});
  const auto read = readClusterCache(file, 11);
  std::filesystem::remove(file);
  REQUIRE(read);
  CHECK(read->front().addedSources == lod.addedSources);
  REQUIRE(read->front().addedPositions.size() == lod.addedPositions.size());
  CHECK(std::memcmp(read->front().addedPositions.data(), lod.addedPositions.data(),
                    lod.addedPositions.size() * sizeof(Vec3)) == 0);
}

namespace {

// A 6 m crown of disjoint leaf quads around (0, 5, 0) on a 4 m trunk
// cylinder: what the DAG voxel levels are for.
MeshData leafCrown(uint32_t leaves, uint32_t seed, uint32_t tessellation = 1) {
  MeshData mesh;
  uint32_t state = seed * 747796405u + 2891336453u;
  const auto rnd = [&] {
    state = state * 747796405u + 2891336453u;
    return float((state >> 8) & 0xFFFFFF) / float(0x1000000);
  };
  for (uint32_t i = 0; i < leaves; ++i) {
    Vec3 c{rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1};
    if (length(c) > 1.0f) {
      --i;
      continue;
    }
    c = Vec3{c.x * 3.0f, 5.0f + c.y * 3.0f, c.z * 3.0f};
    const Vec3 n = normalize(Vec3{rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1 + 1e-3f});
    const Vec3 a = normalize(cross(n, Vec3{0.31f, 0.53f, 0.79f}));
    const Vec3 b = cross(n, a);
    const float s = 0.08f + 0.06f * rnd();
    // A leaf of tessellation^2 quads, as traced leaves are many triangles.
    const uint32_t base = uint32_t(mesh.vertices.size());
    const uint32_t side = tessellation + 1;
    for (uint32_t y = 0; y < side; ++y) {
      for (uint32_t x = 0; x < side; ++x) {
        Vertex v;
        const float u = float(x) / tessellation, w = float(y) / tessellation;
        // Curved, as real leaves are: flat ones simplify without error.
        const float bend = 0.5f * s * ((2.0f * u - 1.0f) * (2.0f * u - 1.0f) +
                                       (2.0f * w - 1.0f) * (2.0f * w - 1.0f));
        v.position = c + a * (s * (2.0f * u - 1.0f)) + b * (s * (2.0f * w - 1.0f)) + n * bend;
        v.normal = n;
        v.uvX = u;
        v.uvY = w;
        mesh.vertices.push_back(v);
      }
    }
    for (uint32_t y = 0; y < tessellation; ++y) {
      for (uint32_t x = 0; x < tessellation; ++x) {
        const uint32_t i = base + y * side + x;
        mesh.indices.insert(mesh.indices.end(), {i, i + 1, i + side + 1, i, i + side + 1, i + side});
      }
    }
  }
  mesh.leaf = true;
  mesh.voxelize = true;
  return mesh;
}

const ClusterLod &cachedCrown() {
  static const VoxelMaterial material{};
  static const ClusterLod lod = [] {
    const MeshData mesh = leafCrown(6000, 1);
    const VoxelSource source{{&material, 1}};
    return buildClusterLod(mesh, &source);
  }();
  return lod;
}

size_t cachedCrownVertexCount() {
  static const size_t count = leafCrown(6000, 1).vertices.size();
  return count;
}

bool isVoxel(const Cluster &c) { return c.brickCount > 0; }

} // namespace

TEST_CASE("voxel levels appear above a crown's triangles and stay voxels") {
  const ClusterLod &lod = cachedCrown();
  size_t voxels = 0;
  for (const Cluster &c : lod.clusters) {
    voxels += isVoxel(c);
  }
  CHECK(voxels > 0);
  CHECK(!lod.bricks.empty());
  // A triangle cluster is never made from voxels.
  for (const Cluster &c : lod.clusters) {
    if (isVoxel(c) || c.refinedGroup < 0) {
      continue;
    }
    for (const Cluster &child : lod.clusters) {
      if (int32_t(child.group) == c.refinedGroup) {
        CHECK(!isVoxel(child));
      }
    }
  }
  // Voxel errors are their lattice edges: powers of two.
  for (const Cluster &c : lod.clusters) {
    if (isVoxel(c)) {
      CHECK(std::exp2(std::round(std::log2(c.voxelEdge))) == c.voxelEdge);
      CHECK(c.refined.error >= c.voxelEdge); // the leaf-area move may raise it
    }
  }
}

TEST_CASE("the mixed DAG is consistent and monotonic, so the cut covers the surface once") {
  // lodVisible draws a group's members while its bounds project over the
  // threshold and its replacements once they don't: exactly one side is
  // drawn if every member shares the group's bounds, every replacement
  // shares them as its refined bounds, and no cluster's own error projects
  // larger than its group's from any camera.
  const ClusterLod &lod = cachedCrown();
  std::map<uint32_t, LodBounds> groupBounds;
  for (const Cluster &c : lod.clusters) {
    const auto [it, inserted] = groupBounds.emplace(c.group, c.self);
    CHECK(std::memcmp(&it->second, &c.self, sizeof(LodBounds)) == 0);
  }
  for (const Cluster &c : lod.clusters) {
    if (c.refinedGroup < 0) {
      continue;
    }
    REQUIRE(groupBounds.count(uint32_t(c.refinedGroup)) == 1);
    CHECK(std::memcmp(&groupBounds[uint32_t(c.refinedGroup)], &c.refined, sizeof(LodBounds)) == 0);
    for (float distance : {0.5f, 5.0f, 20.0f, 100.0f, 500.0f, 3000.0f}) {
      for (const Vec3 dir : {Vec3{0, 0, 1}, normalize(Vec3{1, 1, 0})}) {
        const Vec3 camera = Vec3{0.0f, 5.0f, 0.0f} + dir * distance;
        CHECK(projectedErrorPixels(c.self, camera, 1.0f, 1.2f, 0.1f, 1440.0f) >=
              projectedErrorPixels(c.refined, camera, 1.0f, 1.2f, 0.1f, 1440.0f));
      }
    }
  }
}

TEST_CASE("every voxel root switches no later than its triangles") {
  // A voxel root draws while its self projects over kVoxelPixels x the
  // threshold, its triangles while their group projects over the threshold:
  // both draw unless the first condition implies the second.
  const ClusterLod &lod = cachedCrown();
  std::set<uint32_t> triangleGroups;
  for (const Cluster &c : lod.clusters) {
    if (!isVoxel(c)) {
      triangleGroups.insert(c.group);
    }
  }
  size_t roots = 0;
  for (const Cluster &c : lod.clusters) {
    if (!isVoxel(c) || c.refinedGroup < 0 || !triangleGroups.count(uint32_t(c.refinedGroup))) {
      continue;
    }
    ++roots;
    for (float distance : {20.0f, 60.0f, 200.0f, 700.0f, 3000.0f}) {
      for (const Vec3 dir : {Vec3{0, 0, 1}, normalize(Vec3{1, 1, 0})}) {
        const Vec3 camera = Vec3{0.0f, 5.0f, 0.0f} + dir * distance;
        CHECK(projectedErrorPixels(c.self, camera, 1.0f, 1.2f, 0.1f, 1440.0f) >=
              kVoxelPixels * projectedErrorPixels(c.refined, camera, 1.0f, 1.2f, 0.1f, 1440.0f));
      }
    }
  }
  CHECK(roots > 0);
}

TEST_CASE("selectClustersBvh equals lodVisible over mixed levels") {
  checkTraversal(cachedCrown(), {0.0f, 5.0f, 0.0f});
}

TEST_CASE("a voxel cluster's error scales with instance scale like a triangle cluster's") {
  const ClusterLod &lod = cachedCrown();
  const auto voxel = std::find_if(lod.clusters.begin(), lod.clusters.end(), isVoxel);
  REQUIRE(voxel != lod.clusters.end());
  const Vec3 camera{0.0f, 5.0f, 5000.0f};
  const float one = projectedErrorPixels(voxel->refined, camera, 1.0f, 1.2f, 0.1f, 1440.0f);
  const float two = projectedErrorPixels(voxel->refined, camera, 2.0f, 1.2f, 0.1f, 1440.0f);
  CHECK(two == doctest::Approx(2.0f * one).epsilon(0.01));
}

TEST_CASE("a mesh that does not opt in gets no voxels") {
  static const VoxelMaterial material{};
  MeshData crown = leafCrown(2000, 4);
  crown.voxelize = false;
  const VoxelSource source{{&material, 1}};
  const ClusterLod lod = buildClusterLod(crown, &source);
  CHECK(lod.bricks.empty());
  for (const Cluster &c : lod.clusters) {
    CHECK(!isVoxel(c));
  }
}

TEST_CASE("the cluster source key changes when a voxelized texture changes") {
  std::vector<MeshData> meshes{leafCrown(10, 2)};
  std::vector<std::vector<uint8_t>> textures{{1, 2, 3, 4}};
  const VoxelMaterial material{};
  const uint32_t primitiveMaterial = 0;
  const std::array<int, 3> imageOf{0, -1, -1};
  const auto key = [&] {
    return clusterSourceKey(meshes, textures, {&material, 1}, {&primitiveMaterial, 1},
                            {&imageOf, 1});
  };
  const uint64_t a = key();
  textures[0][2] = 9;
  CHECK(key() != a);
  meshes[0].voxelize = false;
  textures[0][2] = 3;
  CHECK(key() != a);
}

TEST_CASE("the cluster source key changes when a voxelized material changes") {
  const std::vector<MeshData> meshes{leafCrown(10, 2)};
  const std::vector<std::vector<uint8_t>> textures{{1, 2, 3, 4}};
  std::vector<VoxelMaterial> materials(2);
  std::vector<uint32_t> primitiveMaterial{0};
  std::vector<std::array<int, 3>> imageOf{{0, -1, -1}, {-1, -1, -1}};
  const auto key = [&] {
    return clusterSourceKey(meshes, textures, materials, primitiveMaterial, imageOf);
  };
  const uint64_t a = key();
  const auto changes = [&](auto &field, auto value) {
    const auto old = field;
    field = value;
    const bool changed = key() != a;
    field = old;
    return changed;
  };
  CHECK(changes(materials[0].baseColorFactor.y, 0.5f));
  CHECK(changes(materials[0].alphaMask, true));
  CHECK(changes(materials[0].alphaCutoff, 0.3f));
  CHECK(changes(materials[0].roughnessFactor, 0.7f));
  CHECK(changes(materials[0].diffuseTransmission, 0.4f));
  CHECK(changes(materials[0].transmissionColor.z, 0.2f));
  CHECK(changes(primitiveMaterial[0], 1u));
  CHECK(changes(imageOf[0][0], -1));
  CHECK(changes(imageOf[1][1], 0));
  CHECK(key() == a);
}

TEST_CASE("voxel bricks and axis depths round-trip the cluster cache") {
  std::vector<ClusterLod> lods{cachedCrown()};
  bakeModelAxisDepths(lods);
  const auto file = cacheFile("meridian_voxel_round_trip.clusters");
  writeClusterCache(file, 7, lods);
  const auto read = readClusterCache(file, 7);
  REQUIRE(read);
  CHECK(std::memcmp((*read)[0].bricks.data(), lods[0].bricks.data(),
                    lods[0].bricks.size() * sizeof(VoxelBrickRef)) == 0);
  CHECK(std::memcmp((*read)[0].voxels.data(), lods[0].voxels.data(),
                    lods[0].voxels.size() * sizeof(PackedVoxel)) == 0);
  CHECK((*read)[0].voxelAxis == lods[0].voxelAxis);
  std::filesystem::remove(file);
}

TEST_CASE("a crown voxel's optical depth out of the model grows toward the crown's centre") {
  std::vector<ClusterLod> lods{cachedCrown()};
  bakeModelAxisDepths(lods);
  REQUIRE(lods[0].voxelAxis.size() == lods[0].voxels.size());
  // Over the finest voxel level: voxels on the +x side see more crown toward
  // -x (the axis depth nibble 1) than toward +x (nibble 0).
  float edge = FLT_MAX;
  for (const Cluster &c : lods[0].clusters) {
    if (isVoxel(c)) {
      edge = std::min(edge, c.voxelEdge);
    }
  }
  double towardCentre = 0, outward = 0;
  for (const Cluster &c : lods[0].clusters) {
    if (!isVoxel(c) || c.voxelEdge != edge) {
      continue;
    }
    for (uint32_t b = c.firstBrick; b < c.firstBrick + c.brickCount; ++b) {
      const VoxelBrickRef &brick = lods[0].bricks[b];
      uint32_t voxel = brick.firstVoxel;
      for (uint32_t i = 0; i < 64; ++i) {
        if (!((brick.occupied >> i) & 1u)) {
          continue;
        }
        const float x = (brick.origin[0] + float(i % 4) + 0.5f) * edge;
        const uint32_t axis = lods[0].voxelAxis[voxel++];
        if (x > 1.5f) {
          outward += axis & 0xF;
          towardCentre += (axis >> 4) & 0xF;
        }
      }
    }
  }
  CHECK(towardCentre > 2.0 * outward);
}

TEST_CASE("a mesh's voxel levels converge: its coarsest voxels are a handful of clusters") {
  // Every group that goes to voxels joins one hierarchy per mesh, so a far
  // tree draws a few clusters, not one per group (the Megascans beech's
  // leaves had 17475 voxel roots and, as separate subtrees, 1650 terminal
  // clusters).
  size_t terminal = 0;
  for (const Cluster &c : cachedCrown().clusters) {
    terminal += isVoxel(c) && c.self.error == FLT_MAX;
  }
  CHECK(terminal >= 1);
  CHECK(terminal <= 8);
}

TEST_CASE("no voxel is finer than the mesh's height / 512") {
  const ClusterLod &lod = cachedCrown();
  for (const Cluster &c : lod.clusters) {
    if (isVoxel(c)) {
      CHECK(c.voxelEdge >= 6.0f / 512.0f / 2.0f); // the crown is ~6 m tall; edges are powers of two
    }
  }
}

TEST_CASE("voxels hidden from every direction are dropped from the bricks") {
  std::vector<ClusterLod> lods{cachedCrown()};
  size_t before = lods[0].voxels.size();
  bakeModelAxisDepths(lods);
  const ClusterLod &lod = lods[0];
  CHECK(lod.voxels.size() <= before);
  REQUIRE(lod.voxelAxis.size() == lod.voxels.size());
  CHECK(validClusterLod(lod, cachedCrownVertexCount()));
  // Kept voxels are visible from some direction, except the one a cluster
  // keeps when none of its voxels is.
  size_t hiddenKept = 0, voxelClusters = 0;
  for (uint32_t axis : lod.voxelAxis) {
    float least = 1e9f;
    for (int k = 0; k < 6; ++k) {
      least = std::min(least, float((axis >> (4 * k)) & 15u) * kVoxelAxisDepthStep);
    }
    hiddenKept += least > kVoxelHiddenDepth;
  }
  for (const Cluster &c : lod.clusters) {
    voxelClusters += isVoxel(c);
  }
  CHECK(hiddenKept <= voxelClusters);
  CHECK(lod.voxels.size() < before); // a 6 m crown has an inside
}
