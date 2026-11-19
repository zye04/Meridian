#include <doctest/doctest.h>
#include <meridian/placement.h>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace meridian;

namespace {

// Plane rising east at slopeDeg, centered on the origin.
Heightfield planeField(uint32_t size, float spacing, float slopeDeg) {
  std::vector<float> heights(static_cast<size_t>(size) * size);
  const float rise = std::tan(slopeDeg * 3.14159265f / 180.0f) * spacing;
  for (uint32_t j = 0; j < size; ++j) {
    for (uint32_t i = 0; i < size; ++i) {
      heights[static_cast<size_t>(j) * size + i] = 500.0f + rise * static_cast<float>(i);
    }
  }
  if (slopeDeg == 0.0f) {
    heights[0] = 400.0f; // quantizeHeightfield needs a nonzero range
  }
  return quantizeHeightfield(heights, size, size, spacing);
}

DensityRule openRule() {
  return {0.0f, 90.0f, -1e9f, 1e9f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
}

PlacementLayer makeLayer(const DensityRule &rule) {
  PlacementLayer layer{};
  layer.id = 3;
  layer.prototypes = {{7, 1.0f}, {9, 3.0f}};
  layer.footprint = 4.0f;
  layer.rule = rule;
  layer.scaleMin = 0.8f;
  layer.scaleMax = 1.2f;
  layer.rotation = Rotation::Yaw;
  layer.sinkDepth = 0.3f;
  return layer;
}

int64_t torusDistance2(uint32_t a, uint32_t b) {
  int64_t d2 = 0;
  for (int shift : {0, 16}) {
    const int64_t d = ((a >> shift) - (b >> shift)) & 0xFFFF;
    const int64_t w = std::min(d, 65536 - d);
    d2 += w * w;
  }
  return d2;
}

int64_t fixed(float meters) { return static_cast<int64_t>(meters * 65536.0f); }

} // namespace

TEST_CASE("placementHash is pinned (the GPU mirror must match)") {
  CHECK(placementHash(0, 0, 0) == 0x9bafd7c6u);
  CHECK(placementHash(1, 2, 3) == 0xfa9f79a6u);
  CHECK(placementHash(0xFFFFFFFFu, 7, 123456789) == 0xaf5efd19u);
}

TEST_CASE("makeDitherPattern is deterministic and ranks every point once") {
  const DitherPattern a = makeDitherPattern(5, 256);
  const DitherPattern b = makeDitherPattern(5, 256);
  CHECK(a.points == b.points);
  CHECK(a.thresholds == b.thresholds);
  CHECK(a.points != makeDitherPattern(6, 256).points);
  REQUIRE(a.points.size() == 256);
  REQUIRE(a.thresholds.size() == 256);
  std::vector<uint16_t> sorted = a.thresholds;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
  CHECK(sorted.back() < 65535);
}

TEST_CASE("every threshold prefix of the dither pattern stays even") {
  const uint32_t count = 256;
  const DitherPattern pattern = makeDitherPattern(11, count);
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return pattern.thresholds[a] < pattern.thresholds[b]; });
  for (uint32_t percent : {10u, 50u, 100u}) {
    const uint32_t kept = count * percent / 100;
    int64_t min2 = INT64_MAX;
    for (uint32_t a = 0; a < kept; ++a) {
      for (uint32_t b = a + 1; b < kept; ++b) {
        min2 = std::min(min2, torusDistance2(pattern.points[order[a]], pattern.points[order[b]]));
      }
    }
    const double spacing = 65536.0 / std::sqrt(static_cast<double>(kept));
    CAPTURE(percent);
    CHECK(std::sqrt(static_cast<double>(min2)) >= 0.5 * spacing);
  }
}

TEST_CASE("densityAt is 0 outside the slope range and full deep inside") {
  DensityRule rule = openRule();
  rule.slopeMinDeg = 5.0f;
  rule.slopeMaxDeg = 35.0f;
  const PlacementLayer layer = makeLayer(rule);
  CHECK(densityAt(planeField(65, 2.0f, 0.0f), layer, 1, fixed(10), fixed(3)) == 0);
  CHECK(densityAt(planeField(65, 2.0f, 50.0f), layer, 1, fixed(10), fixed(3)) == 0);
  CHECK(densityAt(planeField(65, 2.0f, 20.0f), layer, 1, fixed(10), fixed(3)) == 65535);
  CHECK(densityAt(planeField(65, 2.0f, 0.0f), makeLayer(openRule()), 1, fixed(10), fixed(3)) ==
        65535);
}

TEST_CASE("a negative mask edge is the exact complement of the mask") {
  const Heightfield field = planeField(129, 2.0f, 0.0f);
  DensityRule inside = openRule();
  inside.maskFrequency = 1.0f / 40.0f;
  inside.maskThreshold = 0.5f;
  inside.maskEdge = 0.05f;
  DensityRule outside = inside;
  outside.maskEdge = -0.05f;
  int partial = 0;
  for (int z = -120; z <= 120; z += 3) {
    for (int x = -120; x <= 120; x += 3) {
      const uint16_t a = densityAt(field, makeLayer(inside), 4, fixed(x), fixed(z));
      const uint16_t b = densityAt(field, makeLayer(outside), 4, fixed(x), fixed(z));
      CHECK(a + b == 0xFFFF);
      partial += (a > 0 && b > 0);
    }
  }
  CHECK(partial > 0);
}

TEST_CASE("densityAt values are pinned (anchors for the GPU comparison)") {
  const Heightfield field = planeField(129, 2.0f, 10.0f);
  DensityRule rule = openRule();
  rule.slopeMinDeg = 2.0f;
  rule.slopeMaxDeg = 40.0f;
  rule.maskFrequency = 1.0f / 50.0f;
  rule.maskThreshold = 0.4f;
  rule.maskEdge = 0.2f;
  rule.clusterFrequency = 1.0f / 20.0f;
  rule.clusterAmount = 0.5f;
  const PlacementLayer layer = makeLayer(rule);
  CHECK(densityAt(field, layer, 8, fixed(10.5f), fixed(-30.25f)) == 39852);
  CHECK(densityAt(field, layer, 8, fixed(-77.0f), fixed(41.75f)) == 7760);
  CHECK(densityAt(field, layer, 8, fixed(3.0f), fixed(100.0f)) == 52583);
}

TEST_CASE("prototype weights must sum to a positive total") {
  PlacementLayer layer = makeLayer(openRule());
  layer.prototypes = {{7, 0.0f}};
  CHECK_THROWS(prototypeThresholds(layer));
}

TEST_CASE("evaluatePoint is deterministic and stays inside the crop") {
  const Heightfield field = planeField(101, 2.0f, 10.0f); // crop is +-100 m
  DensityRule rule = openRule();
  rule.maskFrequency = 1.0f / 50.0f;
  rule.maskThreshold = 0.4f;
  rule.maskEdge = 0.1f;
  rule.clusterFrequency = 1.0f / 20.0f;
  rule.clusterAmount = 0.5f;
  const PlacementLayer layer = makeLayer(rule);
  const DitherPattern pattern = makeDitherPattern(2, ditherPointCount(layer.footprint));
  uint32_t kept = 0, total = 0;
  for (int32_t cz = -3; cz <= 2; ++cz) {
    for (int32_t cx = -3; cx <= 2; ++cx) {
      for (uint32_t p = 0; p < pattern.points.size(); ++p) {
        ++total;
        const auto a = evaluatePoint(field, layer, pattern, 8, cx, cz, p);
        const auto b = evaluatePoint(field, layer, pattern, 8, cx, cz, p);
        REQUIRE(a.has_value() == b.has_value());
        if (!a) {
          continue;
        }
        ++kept;
        CHECK(a->position == b->position);
        CHECK(a->scale == b->scale);
        CHECK(a->prototype == b->prototype);
        CHECK(std::abs(a->position.x) <= 100.0f);
        CHECK(std::abs(a->position.z) <= 100.0f);
        CHECK(a->position.y == doctest::Approx(field.heightAt(a->position.x, a->position.z) -
                                               layer.sinkDepth));
        CHECK((a->prototype == 7 || a->prototype == 9));
        CHECK(a->scale >= 0.4f * layer.scaleMin);
        CHECK(a->scale <= layer.scaleMax);
      }
    }
  }
  CHECK(kept > 0);
  CHECK(kept < total);
}

TEST_CASE("an all-reject layer keeps nothing") {
  const Heightfield field = planeField(101, 2.0f, 10.0f);
  DensityRule rule = openRule();
  rule.altitudeMin = 5000.0f;
  rule.altitudeMax = 6000.0f;
  const PlacementLayer layer = makeLayer(rule);
  const DitherPattern pattern = makeDitherPattern(2, ditherPointCount(layer.footprint));
  uint32_t kept = 0;
  for (int32_t cz = -2; cz <= 1; ++cz) {
    for (int32_t cx = -2; cx <= 1; ++cx) {
      for (uint32_t p = 0; p < pattern.points.size(); ++p) {
        kept += evaluatePoint(field, layer, pattern, 8, cx, cz, p).has_value();
      }
    }
  }
  CHECK(kept == 0);
}
