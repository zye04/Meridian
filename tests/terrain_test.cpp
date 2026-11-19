#include <doctest/doctest.h>
#include <meridian/terrain.h>

TEST_CASE("terrainHeight matches mesh vertices") {
  const meridian::TerrainSettings settings;
  const meridian::MeshData mesh = meridian::generateTerrain(settings);
  const int side = settings.resolution + 1;
  const int samples[][2] = {{0, 0}, {256, 300}, {512, 512}, {17, 401}};
  for (const auto &[x, z] : samples) {
    const meridian::Vec3 p = mesh.vertices[z * side + x].position;
    CHECK(meridian::terrainHeight(settings, p.x, p.z) ==
          doctest::Approx(p.y).epsilon(1e-4));
  }
}

TEST_CASE("terrainHeight interpolates between vertices") {
  const meridian::TerrainSettings settings;
  const meridian::MeshData mesh = meridian::generateTerrain(settings);
  const int side = settings.resolution + 1;
  const meridian::Vec3 a = mesh.vertices[300 * side + 256].position;
  const meridian::Vec3 b = mesh.vertices[300 * side + 257].position;
  CHECK(meridian::terrainHeight(settings, (a.x + b.x) / 2, a.z) ==
        doctest::Approx((a.y + b.y) / 2).epsilon(1e-4));
}
