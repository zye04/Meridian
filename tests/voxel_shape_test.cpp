#include <doctest/doctest.h>
#include <meridian/voxel_shape.h>

#include <algorithm>
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

TEST_CASE("every coverage rule keeps a voxel's expected covered area") {
  // Expected covered area = a x the share of noise values that draw, over a
  // fine noise grid; Phase above a pixel and Mixed, Area are checked alike.
  constexpr int kSamples = 100000;
  for (VoxelCoverage rule : {VoxelCoverage::Hash, VoxelCoverage::Area, VoxelCoverage::AreaOffset,
                             VoxelCoverage::Mixed, VoxelCoverage::Phase, VoxelCoverage::AreaPixel}) {
    for (float pixels : {0.5f, 2.0f, 4.0f, 30.0f}) {
      for (float f : {0.0f, 0.01f, 0.1f, 0.25f, 0.49f, 0.75f, 1.0f}) {
        int drawn = 0;
        for (int s = 0; s < kSamples; ++s) {
          const float noise = (static_cast<float>(s) + 0.5f) / kSamples;
          drawn += voxelCoverageDraws(rule, f, false, noise, std::max(pixels, kPhaseVoxelPixels), 0, 8, 0);
        }
        const float covered = voxelAreaFraction(rule, f, pixels) * static_cast<float>(drawn) / kSamples;
        CHECK(covered == doctest::Approx(f).epsilon(0.001));
      }
    }
  }
}

TEST_CASE("the hash rule keeps the opaque floor, the others do not") {
  CHECK(voxelCoverageDraws(VoxelCoverage::Hash, 0.6f, true, 0.9f, 4.0f, 0, 8, 0));
  CHECK_FALSE(voxelCoverageDraws(VoxelCoverage::Mixed, 0.6f, true, 0.9f, 4.0f, 0, 8, 0));
  CHECK_FALSE(voxelCoverageDraws(VoxelCoverage::Phase, 0.6f, true, 0.9f, 4.0f, 0, 8, 0));
}

TEST_CASE("a sub-pixel voxel draws on round(P f) evenly spread jitter phases") {
  constexpr uint32_t kCycle = 8;
  for (float f : {0.0f, 0.05f, 0.1f, 0.3f, 0.5f, 0.62f, 0.9f, 1.0f}) {
    const uint32_t expected = static_cast<uint32_t>(std::lround(kCycle * f));
    for (uint32_t offset : {0u, 3u, 7u, 12345u}) {
      uint32_t drawn = 0, longestGap = 0, gap = 0;
      // Two cycles, so a gap across the cycle's end counts whole.
      for (uint32_t phase = 0; phase < 2 * kCycle; ++phase) {
        const bool draws = voxelCoverageDraws(VoxelCoverage::Phase, f, false, 0.99f, 0.5f, phase % kCycle, kCycle, offset);
        drawn += phase < kCycle && draws;
        gap = draws ? 0 : gap + 1;
        longestGap = std::max(longestGap, gap);
      }
      CHECK(drawn == expected);
      if (expected > 0) {
        // Even: no gap longer than the cycle over the draws, rounded up.
        CHECK(longestGap <= (kCycle + expected - 1) / expected);
      }
    }
  }
}

TEST_CASE("an offset quad stays inside the voxel's full quad") {
  for (float f : {0.0f, 0.04f, 0.3f, 0.81f, 1.0f}) {
    const float half = std::sqrt(voxelAreaFraction(VoxelCoverage::AreaOffset, f, 4.0f));
    for (float u : {0.0f, 0.25f, 0.5f, 0.999999f}) {
      const float offset = voxelQuadOffset(VoxelCoverage::AreaOffset, f, u);
      CHECK(std::abs(offset) + half <= 1.0f + 1e-6f);
    }
    CHECK(voxelQuadOffset(VoxelCoverage::Area, f, 0.9f) == 0.0f);
  }
}

TEST_CASE("an area-pixel quad is never under one pixel, and its phase sequence keeps the coverage") {
  constexpr uint32_t kCycle = 8;
  for (float pixels : {0.5f, 1.0f, 2.5f, 12.0f}) {
    for (float f : {0.02f, 0.1f, 0.3f, 0.7f, 1.0f}) {
      const float a = voxelAreaFraction(VoxelCoverage::AreaPixelPhase, f, pixels);
      CHECK(a * pixels >= std::min(pixels, 1.0f) - 1e-5f);
      CHECK(a <= 1.0f);
      CHECK(a >= f);
      uint32_t drawn = 0;
      for (uint32_t phase = 0; phase < kCycle; ++phase) {
        drawn += voxelCoverageDraws(VoxelCoverage::AreaPixelPhase, f, false, 0.0f, pixels, phase, kCycle, 5);
      }
      CHECK(drawn == static_cast<uint32_t>(std::lround(kCycle * f / a)));
    }
  }
}
