#include <doctest/doctest.h>
#include <meridian/mesh.h>
#include <meridian/model.h>

#include <vector>

// The extension's example: offset (0, 1), scale 0.5 and a quarter turn map
// the texture onto the image's lower-left quadrant, rotated clockwise.
TEST_CASE("applyUvTransform matches the KHR_texture_transform example") {
  meridian::MeshData mesh;
  const float corners[][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  for (const auto &[u, v] : corners) {
    meridian::Vertex vertex;
    vertex.uvX = u;
    vertex.uvY = v;
    mesh.vertices.push_back(vertex);
  }
  meridian::applyUvTransform(mesh, {{0.0f, 1.0f}, {0.5f, 0.5f}, 1.57079632679f});
  const float expected[][2] = {{0, 1}, {0, 0.5f}, {0.5f, 1}, {0.5f, 0.5f}};
  for (size_t i = 0; i < mesh.vertices.size(); ++i) {
    CHECK(mesh.vertices[i].uvX == doctest::Approx(expected[i][0]));
    CHECK(mesh.vertices[i].uvY == doctest::Approx(expected[i][1]));
  }
}

TEST_CASE("a model voxelizes when a foliage mesh is taller than 4 m") {
  meridian::MeshData crown, trunk;
  for (float y : {0.0f, 5.0f}) {
    meridian::Vertex v;
    v.position = {0.0f, y, 0.0f};
    crown.vertices.push_back(v);
    trunk.vertices.push_back(v);
  }
  std::vector<meridian::MeshData> meshes{crown, trunk};
  CHECK(meridian::voxelizesModel(meshes, std::vector<uint8_t>{1, 0}));
  CHECK(!meridian::voxelizesModel(meshes, std::vector<uint8_t>{0, 0}));
  meshes[0].vertices[1].position.y = 3.0f; // a 3 m plant: grass, ferns, flowers stay triangles
  CHECK(!meridian::voxelizesModel(meshes, std::vector<uint8_t>{1, 0}));
}
