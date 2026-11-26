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

TEST_CASE("wood and the hash rule draw whole quads by the hash, floor included") {
  for (float noise : {0.0f, 0.3f, 0.6f, 0.99f}) {
    for (float f : {0.1f, 0.4f, 0.6f}) {
      CHECK(voxelCoverageDraws(VoxelCoverage::AreaPixelPhase, f, true, noise, 0.5f, 3, 8, 1) ==
            voxelDraws(f, true, noise));
      CHECK(voxelCoverageDraws(VoxelCoverage::Hash, f, false, noise, 0.5f, 3, 8, 1) == voxelDraws(f, false, noise));
    }
  }
  CHECK(voxelDrawsWhole(VoxelCoverage::AreaPixelPhase, true));
  CHECK(voxelDrawsWhole(VoxelCoverage::Hash, false));
  CHECK_FALSE(voxelDrawsWhole(VoxelCoverage::AreaPixelPhase, false));
}

TEST_CASE("a draw chance p draws on floor(P p) evenly spread jitter phases, one more by its hash") {
  constexpr uint32_t kCycle = 8;
  for (float p : {0.0f, 0.03f, 0.05f, 0.1f, 0.3f, 0.5f, 0.62f, 0.9f, 1.0f}) {
    const float t = kCycle * p;
    const uint32_t whole = static_cast<uint32_t>(t);
    const float fraction = t - static_cast<float>(whole);
    for (float noise : {0.0f, 0.2f, 0.5f, 0.97f}) {
      const uint32_t expected = std::min(whole + (noise < fraction ? 1u : 0u), kCycle);
      for (uint32_t offset : {0u, 3u, 7u, 12345u}) {
        uint32_t drawn = 0, longestGap = 0, gap = 0;
        // Two cycles, so a gap across the cycle's end counts whole.
        for (uint32_t phase = 0; phase < 2 * kCycle; ++phase) {
          const bool draws = voxelPhaseDraws(p, phase % kCycle, kCycle, offset, noise);
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
}

TEST_CASE("a leaf quad is never under one pixel, and its phase sequence keeps the coverage") {
  // The draws per cycle over the hash's noise, averaged over a uniform grid
  // of noise values: a p must come out as f, also where P p < 1/2 (which a
  // rounded count would never draw) and never above P draws.
  constexpr uint32_t kNoiseSteps = 4096;
  for (uint32_t cycle : {8u, 32u, 256u}) {
    for (float pixels : {0.0f, 0.5f, 1.0f, 2.5f, 12.0f, 100.0f, 1000.0f}) {
      for (float f : {0.0f, 0.001f, 0.01f, 0.02f, 0.05f, 0.1f, 0.3f, 0.5f, 0.7f, 0.99f, 1.0f}) {
        const float a = voxelAreaFraction(f, pixels);
        if (pixels > 0.0f) {
          CHECK(a * pixels >= std::min(pixels, 1.0f) - 1e-5f);
        }
        CHECK(a <= 1.0f);
        CHECK(a >= f);
        double drawnSum = 0.0;
        for (uint32_t k = 0; k < kNoiseSteps; ++k) {
          const float noise = (static_cast<float>(k) + 0.5f) / kNoiseSteps;
          uint32_t drawn = 0;
          for (uint32_t phase = 0; phase < cycle; ++phase) {
            drawn += voxelCoverageDraws(VoxelCoverage::AreaPixelPhase, f, false, noise, pixels, phase, cycle, 5);
          }
          CHECK(drawn <= cycle);
          drawnSum += drawn;
        }
        const double coverage = a * drawnSum / (static_cast<double>(kNoiseSteps) * cycle);
        // The noise grid resolves the fractional part to 1/kNoiseSteps of a
        // draw; float t = P p adds a few ulps.
        CHECK(std::abs(coverage - f) <= 1.0 / kNoiseSteps + 1e-5);
      }
    }
  }
}
