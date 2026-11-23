#include <doctest/doctest.h>
#include "geo.h"

#include <cmath>
#include <functional>

using meridian::cook::Grid;

namespace {

// x = column * spacing, z = row * spacing (row 0 north).
Grid makeGrid(uint32_t w, uint32_t h, float spacing, const std::function<float(float, float)> &f) {
  Grid g{w, h, spacing, std::vector<float>(size_t{w} * h)};
  for (uint32_t j = 0; j < h; ++j) {
    for (uint32_t i = 0; i < w; ++i) {
      g.v[size_t{j} * w + i] = f(i * spacing, j * spacing);
    }
  }
  return g;
}

float at(const Grid &g, uint32_t i, uint32_t j) { return g.v[size_t{j} * g.width + i]; }

} // namespace

TEST_CASE("fill leaves a monotone slope unchanged") {
  const Grid slope =
      makeGrid(33, 21, 2.0f, [](float x, float z) { return 100.0f - 0.1f * x - 0.03f * z; });
  CHECK(meridian::cook::fillDepressions(slope).v == slope.v);
}

TEST_CASE("fill raises a pit to its spill height") {
  Grid plane = makeGrid(9, 9, 2.0f, [](float x, float) { return 100.0f + 0.01f * x; });
  const float rim = at(plane, 3, 4);
  plane.v[4 * 9 + 4] = 90.0f;
  const Grid filled = meridian::cook::fillDepressions(plane);
  CHECK(at(filled, 4, 4) >= rim);
  CHECK(at(filled, 4, 4) < rim + 0.01f);
}

TEST_CASE("flow converges in a V-valley") {
  // Thalweg along column 20, draining north (toward row 0).
  const Grid valley = makeGrid(41, 41, 2.0f, [](float x, float z) {
    return 100.0f + 0.5f * std::abs(x - 40.0f) + 0.1f * z;
  });
  const Grid sca = meridian::cook::specificCatchmentArea(meridian::cook::fillDepressions(valley));
  CHECK(at(sca, 20, 5) > 10.0f * at(sca, 10, 5));
  CHECK(at(sca, 20, 5) > 10.0f * at(sca, 30, 5));
}

TEST_CASE("flow spreads on a cone") {
  // A peak in the middle; on an ideal cone the specific catchment area at
  // radius r is r / 2 in every direction. D8 would put it all on 8 rays;
  // FD8 stays within ~6% (under one byte of the log2 encoding).
  const float c = 100.0f;
  const Grid cone = makeGrid(101, 101, 2.0f, [c](float x, float z) {
    return 1000.0f - 0.5f * std::hypot(x - c, z - c);
  });
  const Grid sca = meridian::cook::specificCatchmentArea(meridian::cook::fillDepressions(cone));
  const int r = 30;
  const float axis = at(sca, 50 + r, 50);
  for (const auto [di, dj] : {std::pair{-r, 0}, {0, r}, {0, -r}}) {
    CHECK(at(sca, 50 + di, 50 + dj) == doctest::Approx(axis).epsilon(0.1));
  }
  // Diagonal at the same radius, and a direction between axis and diagonal.
  const int d = static_cast<int>(std::lround(r / std::sqrt(2.0f)));
  CHECK(at(sca, 50 + d, 50 + d) == doctest::Approx(axis).epsilon(0.1));
  CHECK(at(sca, 50 + 28, 50 + 11) == doctest::Approx(axis).epsilon(0.1));
}

TEST_CASE("flat plane gives finite wetness") {
  const Grid flat = makeGrid(17, 17, 2.0f, [](float, float) { return 500.0f; });
  const Grid filled = meridian::cook::fillDepressions(flat);
  const Grid twi =
      meridian::cook::wetnessIndex(meridian::cook::specificCatchmentArea(filled), flat);
  for (float v : twi.v) {
    CHECK(std::isfinite(v));
  }
}
