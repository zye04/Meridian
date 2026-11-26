#include <doctest/doctest.h>
#include <meridian/voxel_shape.h>

#include <cmath>
#include <random>

using namespace meridian;

TEST_CASE("a voxel's quad area is finite and bounded for any view direction") {
  std::mt19937 rng(5);
  std::normal_distribution<float> n(0.0f, 1.0f);
  for (int i = 0; i < 10000; ++i) {
    const Vec3 d = normalize(Vec3{n(rng), n(rng), n(rng)} + Vec3{0.0f, 0.0f, 1e-6f});
    const float area = voxelQuadArea(d, 0.5f);
    CHECK(area >= 0.25f - 1e-6f);
    CHECK(area <= std::sqrt(3.0f) * 0.25f + 1e-5f);
  }
}

TEST_CASE("a voxel's opacity along an axis is that axis's") {
  const float depth[3] = {0.7f, 1.5f, 3.0f};
  CHECK(voxelViewOpacity(depth, {1.0f, 0.0f, 0.0f}) == doctest::Approx(1.0f - std::exp(-0.7f)));
  CHECK(voxelViewOpacity(depth, {0.0f, -1.0f, 0.0f}) == doctest::Approx(1.0f - std::exp(-1.5f)));
}

TEST_CASE("a voxel's covered area along a diagonal matches its uniform medium") {
  // Uniform extinction: depth d along every axis. Along a diagonal the
  // chord shortens by S while the area grows by S: for a thin medium the
  // covered area (area x opacity) stays edge^2 x d, the volume's
  // extinction, from every direction.
  const float depth[3] = {0.01f, 0.01f, 0.01f};
  const Vec3 diagonal = normalize(Vec3{1.0f, 1.0f, 1.0f});
  const float along = voxelQuadArea({1.0f, 0.0f, 0.0f}, 1.0f) * voxelViewOpacity(depth, {1.0f, 0.0f, 0.0f});
  const float across = voxelQuadArea(diagonal, 1.0f) * voxelViewOpacity(depth, diagonal);
  CHECK(across == doctest::Approx(along).epsilon(0.01));
}

TEST_CASE("an opaque voxel at or over the coverage floor always draws") {
  // Drawn or dropped whole, coverage f errs by 2 f (1 - f) through the hash
  // and by 1 - f drawn always: equal at f = 1/2.
  CHECK(2.0f * kOpaqueVoxelFloor * (1.0f - kOpaqueVoxelFloor) == doctest::Approx(1.0f - kOpaqueVoxelFloor));
  for (float noise : {0.0f, 0.3f, 0.6f, 0.99f}) {
    CHECK(voxelDraws(0.5f, true, noise));
    CHECK(voxelDraws(0.9f, true, noise));
    CHECK(voxelDraws(0.4f, true, noise) == (noise < 0.4f));
    CHECK(voxelDraws(0.9f, false, noise) == (noise < 0.9f));
  }
}
