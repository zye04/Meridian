#include <doctest/doctest.h>
#include <meridian/shadow_cascades.h>

#include <cmath>

using namespace meridian;

namespace {

DVec3 point(const CascadeWindow &w, const LightBasis &b, double x, double y, double sunward) {
  // x, y in units of halfSize; sunward = distance toward the sun from center.
  return w.center + b.u * (x * w.halfSize) + b.v * (y * w.halfSize) + b.d * -sunward;
}

struct Clip {
  double x, y, z;
};

Clip project(const Mat4 &m, DVec3 relative) {
  const Vec3 p = transformPoint(m, {static_cast<float>(relative.x), static_cast<float>(relative.y),
                                    static_cast<float>(relative.z)});
  return {p.x, p.y, p.z};
}

const Vec3 kSuns[] = {normalize(Vec3{0.45f, 0.85f, 0.25f}), normalize(Vec3{0.9f, 0.05f, -0.4f}),
                      {0, 1, 0}};

CascadeWindow windowFor(uint32_t i, const LightBasis &b, DVec3 camera, Vec3 sun) {
  return cascadeWindow(i, b, camera, 2000.0, sun);
}

} // namespace

TEST_CASE("cascade radii") {
  CHECK(cascadeRadius(0) == 20.0);
  CHECK(cascadeRadius(4) == 5120.0);
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    CHECK(cascadeHalfSize(i) > 1.25 * cascadeRadius(i));
    const double texel = 2.0 * cascadeHalfSize(i) / kShadowMapSize;
    CHECK(cascadeHalfSize(i) - 1.25 * cascadeRadius(i) == doctest::Approx(4.0 * texel));
  }
}

TEST_CASE("light basis orthonormal") {
  for (const Vec3 sun : kSuns) {
    const LightBasis b = lightBasis(sun);
    CHECK(std::sqrt(dot(b.u, b.u)) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(std::sqrt(dot(b.v, b.v)) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(std::sqrt(dot(b.d, b.d)) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(std::abs(dot(b.u, b.v)) < 1e-12);
    CHECK(std::abs(dot(b.u, b.d)) < 1e-12);
    CHECK(std::abs(dot(b.v, b.d)) < 1e-12);
    const double len = std::sqrt(double(sun.x) * sun.x + double(sun.y) * sun.y +
                                 double(sun.z) * sun.z);
    CHECK(b.d.x == doctest::Approx(-sun.x / len).epsilon(1e-12));
    CHECK(b.d.y == doctest::Approx(-sun.y / len).epsilon(1e-12));
    CHECK(b.d.z == doctest::Approx(-sun.z / len).epsilon(1e-12));
  }
}

TEST_CASE("light basis: zenith sun") {
  const LightBasis b = lightBasis({0, 1, 0});
  CHECK(std::isfinite(b.u.x + b.u.y + b.u.z + b.v.x + b.v.y + b.v.z));
  CHECK(std::abs(dot(b.u, b.v)) < 1e-12);
  CHECK(b.d.y == -1.0);
}

namespace {

void checkSnapping(DVec3 camera) {
  const Vec3 sun = kSuns[0];
  const LightBasis b = lightBasis(sun);
  for (uint32_t i : {0u, 3u}) {
    const double texel = 2.0 * cascadeHalfSize(i) / kShadowMapSize;
    const double cu = dot(camera, b.u);
    const DVec3 mid = camera + b.u * ((std::floor(cu / texel) + 0.5) * texel - cu);
    const CascadeWindow m = windowFor(i, b, mid, sun);
    const CascadeWindow n = windowFor(i, b, mid + b.u * (0.3 * texel), sun);
    CHECK(dot(m.center, b.u) == doctest::Approx(dot(n.center, b.u)).epsilon(1e-12));
    CHECK(dot(m.center, b.v) == doctest::Approx(dot(n.center, b.v)).epsilon(1e-12));
    const CascadeWindow next = windowFor(i, b, mid + b.u * texel, sun);
    CHECK(std::abs(dot(next.center - m.center, b.u) - texel) < 1e-9);
    CHECK(std::abs(dot(next.center - m.center, b.v)) < 1e-9);
  }
}

} // namespace

TEST_CASE("snapping") { checkSnapping({10.3, 50.0, -20.7}); }

TEST_CASE("snapping far from origin") { checkSnapping({7000.0, 1288.2, 7000.0}); }

TEST_CASE("projection maps the box") {
  for (const Vec3 sun : kSuns) {
    const LightBasis b = lightBasis(sun);
    const CascadeWindow w = windowFor(2, b, {100, 1300, -300}, sun);
    const Mat4 m = cascadeProjection(w, b);
    for (double x : {-1.0, 1.0}) {
      for (double y : {-1.0, 1.0}) {
        const Clip nearC = project(m, point(w, b, x, y, w.depthNear) - w.center);
        const Clip farC = project(m, point(w, b, x, y, -w.depthFar) - w.center);
        CHECK(nearC.x == doctest::Approx(x).epsilon(1e-5));
        CHECK(nearC.y == doctest::Approx(y).epsilon(1e-5));
        CHECK(nearC.z == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(farC.x == doctest::Approx(x).epsilon(1e-5));
        CHECK(farC.y == doctest::Approx(y).epsilon(1e-5));
        CHECK(std::abs(farC.z) < 1e-5);
      }
    }
  }
}

TEST_CASE("planes contain the box") {
  const Vec3 sun = kSuns[0];
  const LightBasis b = lightBasis(sun);
  const CascadeWindow w = windowFor(1, b, {100, 1300, -300}, sun);
  const auto planes = cascadePlanes(w, b);
  const auto signedDistance = [&](int plane, DVec3 relative) {
    const auto &p = planes[plane];
    return p[0] * relative.x + p[1] * relative.y + p[2] * relative.z + p[3];
  };
  for (double x : {-0.9, 0.0, 0.9}) {
    for (double y : {-0.9, 0.0, 0.9}) {
      for (double s : {-0.9 * w.depthFar, 0.0, 0.9 * w.depthNear}) {
        for (int p = 0; p < 6; ++p) {
          CHECK(signedDistance(p, point(w, b, x, y, s) - w.center) >= 0.0);
        }
      }
    }
  }
  // 1 m outside each face: -u, +u, -v, +v, sunward, far.
  const DVec3 outside[6] = {point(w, b, -1, 0, 0) - w.center + b.u * -1.0,
                            point(w, b, 1, 0, 0) - w.center + b.u,
                            point(w, b, 0, -1, 0) - w.center + b.v * -1.0,
                            point(w, b, 0, 1, 0) - w.center + b.v,
                            point(w, b, 0, 0, w.depthNear) - w.center + b.d * -1.0,
                            point(w, b, 0, 0, -w.depthFar) - w.center + b.d};
  for (int p = 0; p < 6; ++p) {
    CHECK(signedDistance(p, outside[p]) == doctest::Approx(-1.0).epsilon(1e-4));
  }
}

TEST_CASE("recenter threshold") {
  const Vec3 sun = kSuns[0];
  const LightBasis b = lightBasis(sun);
  const DVec3 camera{0, 1300, 0};
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    const CascadeWindow w = windowFor(i, b, camera, sun);
    const double margin = cascadeHalfSize(i) - cascadeRadius(i);
    CHECK_FALSE(needsRecenter(i, w, b, w.center + b.u * (0.49 * margin)));
    CHECK(needsRecenter(i, w, b, w.center + b.u * (0.51 * margin)));
    CHECK(needsRecenter(i, w, b, w.center + b.v * (-0.51 * margin)));
    CHECK_FALSE(needsRecenter(i, w, b, w.center + b.d * (0.49 * margin)));
    CHECK(needsRecenter(i, w, b, w.center + b.d * (0.51 * margin)));
  }
}

TEST_CASE("ShadowCascades: sun below horizon") {
  const Vec3 sun = normalize(Vec3{0.9f, -0.05f, -0.4f});
  const LightBasis b = lightBasis(sun);
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    const CascadeWindow w = windowFor(i, b, {1119, 1355.9, -431}, sun);
    CHECK(std::isfinite(w.center.x + w.center.y + w.center.z));
    CHECK(std::isfinite(w.depthNear));
    CHECK(w.depthNear <= kShadowRayLength);
    CHECK(w.depthNear >= 0.0);
    const Mat4 m = cascadeProjection(w, b);
    CHECK(std::isfinite(m.m[2][0] + m.m[3][2]));
  }
}

TEST_CASE("cascadeWindow depthNear") {
  const Vec3 sun{0.8660254f, 0.5f, 0.0f};
  const LightBasis b = lightBasis(sun);
  const double H = cascadeHalfSize(1);
  const CascadeWindow w = cascadeWindow(1, b, {0, 1300, 0}, 2000.0, sun);
  const double expected = (2000.0 - (1300.0 - H)) / 0.5 + H;
  CHECK(w.depthNear == doctest::Approx(expected).epsilon(1e-6));
  CHECK(cascadeWindow(1, b, {0, 5000, 0}, 0.0, sun).depthNear == 0.0);
}
