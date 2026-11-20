#include <doctest/doctest.h>
#include <meridian/weather.h>

#include <cmath>
#include <vector>

using meridian::Vec3;

namespace {

constexpr float kPi = 3.14159265f;

Vec3 sunAt(float elevationDegrees, float azimuthDegrees) {
  const float e = elevationDegrees * kPi / 180.0f;
  const float a = azimuthDegrees * kPi / 180.0f;
  return {std::cos(e) * std::sin(a), std::sin(e), -std::cos(e) * std::cos(a)};
}

void checkOrthonormal(const meridian::SunBasis &b, Vec3 sun) {
  for (Vec3 v : {b.right, b.up, b.forward}) {
    CHECK(std::isfinite(v.x));
    CHECK(std::isfinite(v.y));
    CHECK(std::isfinite(v.z));
    CHECK(length(v) == doctest::Approx(1.0f).epsilon(1e-5));
  }
  CHECK(std::abs(dot(b.right, b.up)) < 1e-5f);
  CHECK(std::abs(dot(b.right, b.forward)) < 1e-5f);
  CHECK(std::abs(dot(b.up, b.forward)) < 1e-5f);
  // The light travels away from the sun; right x up points back at it.
  CHECK(dot(b.forward, sun) == doctest::Approx(-1.0f).epsilon(1e-5));
  CHECK(dot(cross(b.right, b.up), sun) == doctest::Approx(1.0f).epsilon(1e-5));
}

} // namespace

TEST_CASE("cloud shadow basis is orthonormal for any sun elevation") {
  for (float elevation : {90.0f, 45.0f, 2.0f, 0.0f, -5.0f, -90.0f}) {
    for (float azimuth : {0.0f, 90.0f, 135.0f, 180.0f, 270.0f}) {
      CAPTURE(elevation);
      CAPTURE(azimuth);
      const Vec3 sun = sunAt(elevation, azimuth);
      checkOrthonormal(meridian::cloudShadowBasis(sun), sun);
    }
  }
}

TEST_CASE("cloud shadow basis doesn't flip as the sun passes overhead") {
  // Sunrise in the east to sunset in the west through the zenith, in 0.5
  // degree steps: neighbouring bases stay close (no flip, no spin).
  meridian::SunBasis previous = meridian::cloudShadowBasis(sunAt(1.0f, 90.0f));
  for (float angle = 1.5f; angle <= 179.0f; angle += 0.5f) {
    const float a = angle * kPi / 180.0f;
    const Vec3 sun{std::cos(a), std::sin(a), 0.0f};
    const meridian::SunBasis b = meridian::cloudShadowBasis(sun);
    CAPTURE(angle);
    CHECK(dot(b.right, previous.right) > 0.999f);
    CHECK(dot(b.up, previous.up) > 0.999f);
    previous = b;
  }
}

TEST_CASE("cloud shadow basis doesn't spin as the sun crosses the north horizon") {
  // Along the horizon through due north, then up through it from below:
  // the sun's paths at dusk and dawn in high latitudes.
  auto sweep = [](auto sunAtStep, float from, float to) {
    meridian::SunBasis previous = meridian::cloudShadowBasis(sunAtStep(from));
    for (float angle = from + 0.5f; angle <= to; angle += 0.5f) {
      const meridian::SunBasis b = meridian::cloudShadowBasis(sunAtStep(angle));
      CAPTURE(angle);
      CHECK(dot(b.right, previous.right) > 0.999f);
      CHECK(dot(b.up, previous.up) > 0.999f);
      previous = b;
    }
  };
  sweep([](float azimuth) { return sunAt(0.0f, azimuth); }, -45.0f, 45.0f);
  sweep([](float elevation) { return sunAt(elevation, 0.0f); }, -10.0f, 10.0f);
}

TEST_CASE("Beer shadow transmittance matches brute-force optical depth") {
  // A cumulus-like column along the light: soft top, dense core, thinning
  // base, 1.5 km thick starting 2 km in; extinction in 1/m, thin enough
  // (optical depth 2.5) that the transmittance spans 1 to 0.08.
  constexpr float kStart = 2000.0f;
  constexpr float kThickness = 1500.0f;
  auto extinction = [&](float depth) {
    const float h = (depth - kStart) / kThickness;
    if (h <= 0.0f || h >= 1.0f) {
      return 0.0f;
    }
    const float top = std::min(h / 0.1f, 1.0f);
    const float base = std::min((1.0f - h) / 0.2f, 1.0f);
    return 0.002f * top * top * (3.0f - 2.0f * top) * base;
  };
  // The map's sampling: 32 steps across the layer envelope (1 km before to
  // 0.5 km past the column), sampled at step centers.
  constexpr int kSteps = 32;
  constexpr float kEnvelopeStart = kStart - 1000.0f;
  constexpr float kStep = (kThickness + 1500.0f) / kSteps;
  std::vector<float> samples(kSteps);
  for (int i = 0; i < kSteps; ++i) {
    samples[i] = extinction(kEnvelopeStart + (static_cast<float>(i) + 0.5f) * kStep);
  }
  const meridian::BeerShadowTexel texel =
      meridian::beerShadowTexel(samples, kEnvelopeStart, kStep);

  // Ten depths through the column, then two well past it (5 and 8 km),
  // where only the max optical depth clamp keeps the shadow right.
  std::vector<float> depths;
  for (int k = 0; k < 10; ++k) {
    depths.push_back(kStart - 200.0f + static_cast<float>(k) * 220.0f);
  }
  depths.push_back(5000.0f);
  depths.push_back(8000.0f);
  for (float depth : depths) {
    double opticalDepth = 0.0;
    for (float d = 0.0f; d < depth; d += 0.1f) {
      opticalDepth += extinction(d + 0.05f) * 0.1;
    }
    const float transmittance = meridian::beerShadowTransmittance(texel, depth);
    CAPTURE(depth);
    CAPTURE(opticalDepth);
    // The model's linear optical depth overestimates where the column thins
    // (up to 7% of its total here); in transmittance that stays within 0.05.
    CHECK(std::abs(transmittance - std::exp(-opticalDepth)) <= 0.05);
  }
}

TEST_CASE("Beer shadow optical depth matches brute force on a uniform column") {
  // The case the format represents exactly up to its sampling: one mean
  // extinction from front to back. Depths run from before the column to
  // 1.5 km past it, where only the max optical depth clamp holds.
  constexpr float kStart = 2000.0f;
  constexpr float kThickness = 1500.0f;
  constexpr float kExtinction = 0.002f;
  auto extinction = [&](float depth) {
    return depth > kStart && depth < kStart + kThickness ? kExtinction : 0.0f;
  };
  constexpr int kSteps = 32;
  constexpr float kEnvelopeStart = kStart - 1000.0f;
  constexpr float kStep = (kThickness + 1500.0f) / kSteps;
  std::vector<float> samples(kSteps);
  for (int i = 0; i < kSteps; ++i) {
    samples[i] = extinction(kEnvelopeStart + (static_cast<float>(i) + 0.5f) * kStep);
  }
  const meridian::BeerShadowTexel texel =
      meridian::beerShadowTexel(samples, kEnvelopeStart, kStep);

  const double total = kExtinction * kThickness;
  for (int k = 0; k < 10; ++k) {
    const float depth = kStart - 200.0f + static_cast<float>(k) * 350.0f;
    double opticalDepth = 0.0;
    for (float d = 0.0f; d < depth; d += 0.1f) {
      opticalDepth += extinction(d + 0.05f) * 0.1;
    }
    const double modelled = -std::log(meridian::beerShadowTransmittance(texel, depth));
    CAPTURE(depth);
    CAPTURE(opticalDepth);
    CAPTURE(modelled);
    CHECK(std::abs(modelled - opticalDepth) <= 0.05 * total);
  }
}
