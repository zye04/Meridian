#include <doctest/doctest.h>
#include "geo.h"

#include <meridian/geo.h>
#include <meridian/terrain.h>

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

TEST_CASE("topographic position is positive on a ridge and negative in a bowl") {
  const Grid ridge =
      makeGrid(41, 41, 2.0f, [](float x, float) { return 100.0f - 0.5f * std::abs(x - 40.0f); });
  const Grid bowl = makeGrid(41, 41, 2.0f, [](float x, float z) {
    return 100.0f + 0.01f * ((x - 40.0f) * (x - 40.0f) + (z - 40.0f) * (z - 40.0f));
  });
  CHECK(at(meridian::cook::topographicPosition(ridge, 10.0f), 20, 20) > 1.0f);
  CHECK(at(meridian::cook::topographicPosition(bowl, 10.0f), 20, 20) < -0.2f);
}

TEST_CASE("south slope gets more sun than north at 47 N") {
  // South = +z (row index grows southward).
  const Grid south = makeGrid(41, 41, 4.0f, [](float, float z) { return 500.0f - 0.4f * z; });
  const Grid north = makeGrid(41, 41, 4.0f, [](float, float z) { return 500.0f + 0.4f * z; });
  const float s = at(meridian::cook::sunExposure(south, 47.0f), 20, 20);
  const float n = at(meridian::cook::sunExposure(north, 47.0f), 20, 20);
  CHECK(s > 1.1f);
  CHECK(n < 0.8f);
  const Grid flat = makeGrid(41, 41, 4.0f, [](float, float) { return 500.0f; });
  CHECK(at(meridian::cook::sunExposure(flat, 47.0f), 20, 20) == doctest::Approx(1.0f));
}

TEST_CASE("sun exposure is finite at any latitude") {
  const Grid cone = makeGrid(41, 41, 4.0f, [](float x, float z) {
    return 1000.0f - 0.8f * std::hypot(x - 80.0f, z - 80.0f);
  });
  for (float latitude : {89.0f, -60.0f, 0.0f}) {
    for (float v : meridian::cook::sunExposure(cone, latitude).v) {
      CHECK(std::isfinite(v));
      CHECK(v >= 0.0f);
    }
  }
}

TEST_CASE("talus lies below a cliff step and not above it") {
  // Plateau in the north, a 30 m cliff at z = 40..44 m, a gentle plain south of it.
  const Grid step = makeGrid(21, 41, 2.0f, [](float, float z) {
    if (z < 40.0f) {
      return 150.0f - 0.01f * z;
    }
    if (z < 44.0f) {
      return 150.0f - 7.5f * (z - 40.0f);
    }
    return 120.0f - 0.05f * (z - 44.0f);
  });
  const Grid t = meridian::cook::talus(step);
  CHECK(at(t, 10, 25) > 0.0f);  // just below the foot
  CHECK(at(t, 10, 10) == 0.0f); // on the plateau
  CHECK(at(t, 10, 21) == 0.0f); // on the cliff face: scree lies below the foot
  CHECK(at(t, 10, 25) > at(t, 10, 38));
}

TEST_CASE("cookGeo refuses a ratio that does not divide the width") {
  const auto field = meridian::quantizeHeightfield(std::vector<float>(10 * 8, 100.0f), 10, 8, 0.5f);
  CHECK_THROWS(meridian::cook::cookGeo(field, 4, 47.0f));
  const meridian::GeoChannels geo = meridian::cook::cookGeo(
      meridian::quantizeHeightfield(std::vector<float>(8 * 8, 100.0f), 8, 8, 0.5f), 4, 47.0f);
  CHECK(geo.width == 2);
  CHECK(geo.ratio == 4);
  CHECK(geo.texels.size() == 2 * 2 * meridian::kGeoChannelCount);
}
