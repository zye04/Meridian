#include <doctest/doctest.h>
#include <meridian/gi.h>

#include <cmath>
#include <numbers>
#include <random>

using namespace meridian;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

std::vector<Vec3> fibonacciDirections(int n) {
  std::vector<Vec3> d;
  const float golden = kPi * (3.0f - std::sqrt(5.0f));
  for (int i = 0; i < n; ++i) {
    const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    const float r = std::sqrt(1.0f - y * y);
    const float a = golden * static_cast<float>(i);
    d.push_back({r * std::cos(a), y, r * std::sin(a)});
  }
  return d;
}

} // namespace

TEST_CASE("gi slice coordinate inverts slice height") {
  for (uint32_t k = 0; k < kGiLevels; ++k) {
    for (uint32_t t = 0; t < kGiSlices; ++t) {
      const float ft = static_cast<float>(t);
      CHECK(giSliceCoordinate(k, giSliceHeight(k, ft)) == doctest::Approx(ft).epsilon(1e-5));
    }
  }
  CHECK(giSliceCoordinate(0, -1.0f) == 0.0f);
  CHECK(giSliceCoordinate(0, 1e6f) == 7.0f);
  CHECK(giSliceHeight(0, 7.0f) == 31.75f);
  CHECK(giSliceHeight(4, 7.0f) == 508.0f);
}

TEST_CASE("gi clipmap origin and toroidal wrap") {
  const auto o = giClipmapOrigin(2.0f, 48, 0.9f, -0.1f);
  CHECK(o[0] == -24);
  CHECK(o[1] == -25);
  const auto far = giClipmapOrigin(2.0f, 48, 1e5f, 0.0f);
  const int32_t column = static_cast<int32_t>(std::floor(1e5f / 2.0f));
  CHECK(far[0] <= column);
  CHECK(column < far[0] + 48);
  CHECK(giToroidal(-1, 48) == 47);
  CHECK(giToroidal(48, 48) == 0);
}

TEST_CASE("gi sh irradiance of uniform radiance is pi L") {
  ShL1 sh;
  const auto dirs = fibonacciDirections(4096);
  for (const Vec3 &d : dirs) {
    addRadianceSample(sh, d, {2.0f, 1.0f, 0.5f}, 4.0f * kPi / 4096.0f);
  }
  const Vec3 axes[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const Vec3 &n : axes) {
    const Vec3 e = shIrradiance(sh, n);
    CHECK(e.x == doctest::Approx(kPi * 2.0f).epsilon(1e-2));
    CHECK(e.y == doctest::Approx(kPi * 1.0f).epsilon(1e-2));
    CHECK(e.z == doctest::Approx(kPi * 0.5f).epsilon(1e-2));
  }
}

TEST_CASE("gi sh irradiance of an upper hemisphere stays non-negative") {
  ShL1 sh;
  const auto dirs = fibonacciDirections(4096);
  for (const Vec3 &d : dirs) {
    if (d.y > 0.0f) {
      addRadianceSample(sh, d, {1.0f, 1.0f, 1.0f}, 4.0f * kPi / 4096.0f);
    }
  }
  CHECK(shIrradiance(sh, {0, 1, 0}).x == doctest::Approx(kPi).epsilon(0.05));
  CHECK(shIrradiance(sh, {0, -1, 0}).x <= 0.1f);
  std::mt19937 rng(7);
  std::normal_distribution<float> gauss;
  for (int i = 0; i < 100; ++i) {
    const Vec3 n = normalize(Vec3{gauss(rng), gauss(rng), gauss(rng)});
    const Vec3 e = shIrradiance(sh, n);
    CHECK(e.x >= 0.0f);
    CHECK(e.y >= 0.0f);
    CHECK(e.z >= 0.0f);
  }
}

TEST_CASE("leafPhase integrates to the scattering albedo") {
  const std::array<std::array<float, 2>, 3> leaves{{{0.1f, 0.1f}, {0.05f, 0.2f}, {0.3f, 0.0f}}};
  for (const auto &[r, t] : leaves) {
    constexpr int kSteps = 4096;
    double sum = 0.0;
    for (int i = 0; i < kSteps; ++i) {
      const double b = (i + 0.5) * kPi / kSteps;
      sum += leafPhase(static_cast<float>(std::cos(b)), r, t) * std::sin(b);
    }
    CHECK(2.0 * kPi * sum * kPi / kSteps == doctest::Approx(r + t).epsilon(1e-3));
  }
}

TEST_CASE("leafPhase forward is transmission, backward is reflection") {
  CHECK(leafPhase(1.0f, 0.0f, 0.2f) > 0.0f);
  CHECK(leafPhase(1.0f, 0.2f, 0.0f) == doctest::Approx(0.0f));
  CHECK(leafPhase(-1.0f, 0.2f, 0.0f) > leafPhase(-1.0f, 0.0f, 0.2f));
}
