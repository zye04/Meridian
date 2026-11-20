#include <doctest/doctest.h>
#include <meridian/placement.h>

#include <algorithm>
#include <array>
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

namespace {

// Angle of the rotation between two unit quaternions (x, y, z, w).
// From the chord between the quaternions (2 sin(angle / 4)), in double:
// acos of their dot product loses small angles to rounding.
float rotationError(const std::array<float, 4> &a, const std::array<float, 4> &b) {
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (int i = 0; i < 4; ++i) {
    dot += double{a[i]} * b[i];
    na += double{a[i]} * a[i];
    nb += double{b[i]} * b[i];
  }
  const double sign = dot < 0.0 ? -1.0 : 1.0;
  double chord2 = 0.0;
  for (int i = 0; i < 4; ++i) {
    const double d = a[i] / std::sqrt(na) - sign * b[i] / std::sqrt(nb);
    chord2 += d * d;
  }
  return static_cast<float>(4.0 * std::asin(std::min(1.0, 0.5 * std::sqrt(chord2))));
}

} // namespace

TEST_CASE("placed instances round-trip through the 24-byte encoding") {
  float worst = 0.0f, worstAccurate = 0.0f;
  for (uint32_t i = 0; i < 20000; ++i) {
    PlacedPoint point{};
    point.position = {-7499.0f + i * 0.75f, 1300.25f + i * 0.001f, 7000.5f - i * 0.5f};
    const float u = static_cast<float>(placementHash(i, 1, 0) & 0xFFFF) / 65535.0f;
    const float v = static_cast<float>(placementHash(i, 2, 0) & 0xFFFF) / 65535.0f;
    if (i % 2 == 0) {
      point.axis = {0.0f, 1.0f, 0.0f}; // trees: yaw only
    } else {
      const float y = 2.0f * v - 1.0f, r = std::sqrt(1.0f - y * y);
      point.axis = {r * std::cos(6.2831853f * u), y, r * std::sin(6.2831853f * u)};
    }
    point.angle = 6.2831853f * static_cast<float>(placementHash(i, 3, 0) & 0xFFFF) / 65535.0f;
    point.scale = 0.2f + 3.0f * u;
    point.prototype = i % 7;

    const DecodedInstance decoded = decodePlacedInstance(encodePlacedInstance(point));
    CHECK(decoded.position == point.position);
    CHECK(decoded.prototype == point.prototype);
    CHECK(std::abs(decoded.scale - point.scale) <= 1e-3f * point.scale);
    const std::array<float, 4> q = placedRotation(point);
    CHECK(q[3] >= 0.0f);
    const float error = rotationError(q, decoded.rotation);
    worst = std::max(worst, error);
    // xyz in snorm16 with w rebuilt from them: the error grows as 1/w.
    if (q[3] >= 0.6f) {
      worstAccurate = std::max(worstAccurate, error);
    }
  }
  CHECK(worstAccurate < 1e-4f);
  CHECK(worst < 0.015f);
}

TEST_CASE("the placement grid covers the crop with whole cells") {
  Heightfield field; // the Napf's size; the grid needs no samples
  field.width = field.height = 7500;
  field.spacing = 2.0f; // crop +-7499 m
  const PlacementGrid grid = placementGrid(field);
  CHECK(grid.firstX == -118);
  CHECK(grid.firstZ == -118);
  CHECK(grid.columns == 236);
  CHECK(grid.rows == 236);
}

TEST_CASE("streamKeepFraction thins by distance squared and fades out") {
  CHECK(streamKeepFraction(0.0f, 40.0f, 200.0f) == 1.0f);
  CHECK(streamKeepFraction(40.0f, 40.0f, 200.0f) == 1.0f);
  CHECK(streamKeepFraction(80.0f, 40.0f, 200.0f) == doctest::Approx(0.25f));
  CHECK(streamKeepFraction(200.0f, 40.0f, 200.0f) == 0.0f);
  CHECK(streamKeepFraction(500.0f, 40.0f, 200.0f) == 0.0f);
  CHECK(streamKeepFraction(100.0f, 40.0f, 200.0f) == doctest::Approx(0.16f));
  const float inFade = streamKeepFraction(150.0f, 40.0f, 200.0f);
  CHECK(inFade == doctest::Approx((40.0f / 150.0f) * (40.0f / 150.0f) * 0.5f));
}

TEST_CASE("the keep fraction is continuous and never rises with distance") {
  float last = 1.0f;
  for (int i = 0; i <= 4000; ++i) {
    const float k = streamKeepFraction(0.05f * i, 40.0f, 200.0f);
    CHECK(k <= last);
    CHECK(last - k < 0.004f);
    last = k;
  }
}

TEST_CASE("a kept point shrinks over a keep margin of 0.3") {
  CHECK(streamFade(0.8f, 0.5f) == 1.0f);
  CHECK(streamFade(0.65f, 0.5f) == doctest::Approx(0.5f));
  CHECK(streamFade(0.55f, 0.5f) < 0.2f);
}

TEST_CASE("streamFade is zero at the threshold and monotonic") {
  CHECK(streamFade(0.3f, 0.3f) == 0.0f);
  CHECK(streamFade(0.2f, 0.5f) == 0.0f);
  CHECK(streamFade(1.0f, 0.0f) == 1.0f);
  float last = 0.0f;
  for (int i = 0; i <= 100; ++i) {
    const float f = streamFade(0.5f + 0.001f * i, 0.5f);
    CHECK(f >= last);
    last = f;
  }
}

TEST_CASE("streamWindowCells") {
  CHECK(streamWindowCells(0.0f) == 1);
  CHECK(streamWindowCells(1.0f) == 3);
  CHECK(streamWindowCells(64.0f) == 3);
  CHECK(streamWindowCells(65.0f) == 5);
  CHECK(streamWindowCells(200.0f) == 9);
}

TEST_CASE("every cell within reach is held while the camera moves") {
  // The update runs in the frame that uses it, so the window must hold
  // every cell within the reach of the camera, whatever its position.
  const float radius = 121.0f;
  const uint32_t window = streamWindowCells(radius);
  for (int step = 0; step < 400; ++step) {
    const float x = -500.0f + 2.5f * static_cast<float>(step);
    const float z = 37.0f - 1.7f * static_cast<float>(step);
    const auto cameraX = static_cast<int32_t>(std::floor(x / kCellSize));
    const auto cameraZ = static_cast<int32_t>(std::floor(z / kCellSize));
    for (int32_t cz = cameraZ - 4; cz <= cameraZ + 4; ++cz) {
      for (int32_t cx = cameraX - 4; cx <= cameraX + 4; ++cx) {
        const float nx = std::clamp(x, cx * kCellSize, (cx + 1) * kCellSize);
        const float nz = std::clamp(z, cz * kCellSize, (cz + 1) * kCellSize);
        if (std::hypot(nx - x, nz - z) > radius) {
          continue;
        }
        const auto [sx, sz] = streamSlot(cx, cz, window);
        CHECK(streamSlotCell(sx, sz, cameraX, cameraZ, window) == std::pair<int32_t, int32_t>{cx, cz});
      }
    }
  }
}

TEST_CASE("streamSlot is a floor mod") {
  CHECK(streamSlot(0, 0, 5) == std::pair<uint32_t, uint32_t>{0, 0});
  CHECK(streamSlot(7, 4, 5) == std::pair<uint32_t, uint32_t>{2, 4});
  CHECK(streamSlot(-1, -5, 5) == std::pair<uint32_t, uint32_t>{4, 0});
  CHECK(streamSlot(-6, -7, 5) == std::pair<uint32_t, uint32_t>{4, 3});
}

TEST_CASE("streamSlotCell inverts streamSlot over the window") {
  for (uint32_t window : {1u, 3u, 9u, 11u}) {
    const int32_t half = static_cast<int32_t>(window / 2);
    for (int32_t cx : {0, 3, -1, -17, 100}) {
      for (int32_t cz : {0, -4, 29, -128}) {
        for (int32_t x = cx - half; x <= cx + half; ++x) {
          for (int32_t z = cz - half; z <= cz + half; ++z) {
            const auto [sx, sz] = streamSlot(x, z, window);
            const auto cell = streamSlotCell(sx, sz, cx, cz, window);
            CHECK(cell.first == x);
            CHECK(cell.second == z);
          }
        }
      }
    }
  }
}
