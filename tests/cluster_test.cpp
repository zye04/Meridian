#include <doctest/doctest.h>
#include <meridian/mesh.h>

#include <cfloat>
#include <cmath>

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
