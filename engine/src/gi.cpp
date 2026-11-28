#include <meridian/gi.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace meridian {

namespace {

constexpr float kY00 = 0.282095f;
constexpr float kY1 = 0.488603f;

float level2(uint32_t level) { return static_cast<float>(1u << level); }

} // namespace

float giSpacing(uint32_t level) { return kGiSpacing0 * level2(level); }

float giSliceBase(uint32_t level) { return kGiSliceBase0 * level2(level); }

float giSliceHeight(uint32_t level, float slice) {
  return giSliceBase(level) * (std::exp2(slice) - 1.0f);
}

float giSliceCoordinate(uint32_t level, float height) {
  if (height <= 0.0f) {
    return 0.0f;
  }
  const float t = std::log2(1.0f + height / giSliceBase(level));
  return std::min(t, static_cast<float>(kGiSlices - 1));
}

std::array<int32_t, 2> giClipmapOrigin(float spacing, int32_t columns, float x, float z) {
  return {static_cast<int32_t>(std::floor(x / spacing)) - columns / 2,
          static_cast<int32_t>(std::floor(z / spacing)) - columns / 2};
}

uint32_t giToroidal(int32_t column, int32_t columns) {
  return static_cast<uint32_t>(((column % columns) + columns) % columns);
}

void addRadianceSample(ShL1 &sh, Vec3 direction, Vec3 radiance, float weight) {
  const float basis[4] = {kY00, kY1 * direction.y, kY1 * direction.z, kY1 * direction.x};
  for (int i = 0; i < 4; ++i) {
    sh.c[i] = sh.c[i] + radiance * (basis[i] * weight);
  }
}

namespace {

// The cosine-convolved L1 irradiance is the linear function E0 + E1 . n
// (E0 = pi Y00 c0, E1 = 2 pi / 3 Y1 c1). It goes negative when |E1| > E0,
// so it is clamped at zero; that is exact for a hemisphere of light (E1 = E0).
// Geomerics' non-negative reconstruction (Hazel, "Reading between the
// lines") was measured instead and overshoots that peak by 17%.
float irradianceChannel(float c0, float cy, float cz, float cx, Vec3 n) {
  constexpr float pi = std::numbers::pi_v<float>;
  const float e0 = pi * kY00 * c0;
  const float e1 = (2.0f * pi / 3.0f) * kY1;
  return std::max(e0 + e1 * (cx * n.x + cy * n.y + cz * n.z), 0.0f);
}

} // namespace

Vec3 shIrradiance(const ShL1 &sh, Vec3 normal) {
  const auto &c = sh.c;
  return {irradianceChannel(c[0].x, c[1].x, c[2].x, c[3].x, normal),
          irradianceChannel(c[0].y, c[1].y, c[2].y, c[3].y, normal),
          irradianceChannel(c[0].z, c[1].z, c[2].z, c[3].z, normal)};
}

std::array<float, 2> leafPhaseWeights(float cosBeta) {
  constexpr float pi = std::numbers::pi_v<float>;
  const float c = std::clamp(cosBeta, -1.0f, 1.0f);
  const float b = std::acos(c);
  const float reflected = (std::sin(b) - b * c) / (3.0f * pi);
  return {reflected / (0.5f * pi), (reflected + c / 3.0f) / (0.5f * pi)};
}

float leafPhase(float cosBeta, float reflectance, float transmittance) {
  const std::array<float, 2> w = leafPhaseWeights(cosBeta);
  return reflectance * w[0] + transmittance * w[1];
}

} // namespace meridian
