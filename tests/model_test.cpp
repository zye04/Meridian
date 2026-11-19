#include <doctest/doctest.h>
#include <meridian/model.h>

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
