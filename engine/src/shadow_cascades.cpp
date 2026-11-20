#include <meridian/shadow_cascades.h>

#include <algorithm>
#include <cmath>

namespace meridian {

namespace {

DVec3 cross(DVec3 a, DVec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

DVec3 normalize(DVec3 a) { return a * (1.0 / std::sqrt(dot(a, a))); }

DVec3 toDouble(Vec3 a) { return {a.x, a.y, a.z}; }

double snapDown(double value, double texel) { return std::floor(value / texel) * texel; }

} // namespace

LightBasis lightBasis(Vec3 sunDirection) {
  const DVec3 d = normalize(toDouble(sunDirection)) * -1.0;
  // Any sun change fully invalidates the cascades, so the basis jump here is harmless.
  const DVec3 up = std::abs(d.y) > 0.99 ? DVec3{0, 0, 1} : DVec3{0, 1, 0};
  const DVec3 u = normalize(cross(d, up));
  const DVec3 v = cross(d, u);
  return {u, v, d};
}

double cascadeRadius(uint32_t i) { return 20.0 * std::pow(4.0, i); }

double cascadeHalfSize(uint32_t i) {
  // H = 1.25 R + 4 texels, texel = 2 H / size.
  return 1.25 * cascadeRadius(i) / (1.0 - 8.0 / kShadowMapSize);
}

CascadeWindow cascadeWindow(uint32_t i, const LightBasis &basis, DVec3 camera,
                            double casterTop, Vec3 sunDirection) {
  CascadeWindow window;
  window.halfSize = cascadeHalfSize(i);
  window.texel = 2.0 * window.halfSize / kShadowMapSize;
  const double u = snapDown(dot(camera, basis.u), window.texel);
  const double v = snapDown(dot(camera, basis.v), window.texel);
  window.center = basis.u * u + basis.v * v + basis.d * dot(camera, basis.d);
  const double sinElevation = sunDirection.y / length(sunDirection);
  window.depthFar = window.halfSize;
  window.depthNear = std::clamp((casterTop - (camera.y - window.halfSize)) /
                                        std::max(sinElevation, 0.02) +
                                    window.halfSize,
                                0.0, kShadowRayLength);
  return window;
}

Mat4 cascadeProjection(const CascadeWindow &window, const LightBasis &basis) {
  const double depth = window.depthNear + window.depthFar;
  const double inverseHalf = 1.0 / window.halfSize;
  Mat4 result;
  const DVec3 rows[3] = {basis.u * inverseHalf, basis.v * inverseHalf,
                         basis.d * (-1.0 / depth)};
  for (int row = 0; row < 3; ++row) {
    result.m[0][row] = static_cast<float>(rows[row].x);
    result.m[1][row] = static_cast<float>(rows[row].y);
    result.m[2][row] = static_cast<float>(rows[row].z);
  }
  result.m[3][2] = static_cast<float>(window.depthFar / depth);
  result.m[3][3] = 1.0f;
  return result;
}

std::array<std::array<float, 4>, 6> cascadePlanes(const CascadeWindow &window,
                                                  const LightBasis &basis) {
  const auto plane = [](DVec3 n, double w) {
    return std::array<float, 4>{static_cast<float>(n.x), static_cast<float>(n.y),
                                static_cast<float>(n.z), static_cast<float>(w)};
  };
  return {plane(basis.u, window.halfSize),
          plane(basis.u * -1.0, window.halfSize),
          plane(basis.v, window.halfSize),
          plane(basis.v * -1.0, window.halfSize),
          plane(basis.d, window.depthNear),
          plane(basis.d * -1.0, window.depthFar)};
}

bool needsRecenter(uint32_t i, const CascadeWindow &rendered, const LightBasis &basis,
                   DVec3 camera) {
  const DVec3 offset = camera - rendered.center;
  const double limit = 0.5 * (cascadeHalfSize(i) - cascadeRadius(i));
  return std::abs(dot(offset, basis.u)) > limit || std::abs(dot(offset, basis.v)) > limit ||
         std::abs(dot(offset, basis.d)) > limit;
}

} // namespace meridian
