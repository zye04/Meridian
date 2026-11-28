#pragma once

#include <meridian/math/vec3.h>

#include <array>
#include <cstdint>

namespace meridian {

inline constexpr uint32_t kGiLevels = 5;
inline constexpr int32_t kGiColumns = 48;
inline constexpr uint32_t kGiSlices = 8;
inline constexpr float kGiSpacing0 = 2.0f;
inline constexpr float kGiSliceBase0 = 0.25f;
inline constexpr uint32_t kGiGroundLevels = 4;
inline constexpr int32_t kGiGroundTexels = 256;
inline constexpr float kGiGroundTexel0 = 1.0f;
inline constexpr uint32_t kGiRays = 64;
inline constexpr float kGiHysteresis = 0.95f;

float giSpacing(uint32_t level);
float giSliceBase(uint32_t level);

// Slice heights above a column base grow geometrically: b (2^t - 1).
float giSliceHeight(uint32_t level, float slice);
// Inverse of giSliceHeight, clamped to [0, kGiSlices - 1].
float giSliceCoordinate(uint32_t level, float height);

// First column index of a window of `columns` centred on (x, z); serves the
// probe columns and the ground cache texels.
std::array<int32_t, 2> giClipmapOrigin(float spacing, int32_t columns, float x, float z);
uint32_t giToroidal(int32_t column, int32_t columns);

// Radiance projected on L1 SH, per colour channel: c[0] is Y00, c[1..3] are
// Y1m ordered {y, z, x}.
struct ShL1 {
  std::array<Vec3, 4> c;
};

void addRadianceSample(ShL1 &sh, Vec3 direction, Vec3 radiance, float weight);
Vec3 shIrradiance(const ShL1 &sh, Vec3 normal);

// Bi-Lambertian leaves with a spherical leaf-angle distribution (Shultis &
// Myneni 1988): the area scattering phase function over pi G (G = 1/2), so
// it integrates over the sphere to reflectance + transmittance. cosBeta is
// between the light's travel direction and the scattered direction.
// The phase per unit reflectance and per unit transmittance (it is linear in
// both); gi.glsl's leafPhaseWeights.
std::array<float, 2> leafPhaseWeights(float cosBeta);
float leafPhase(float cosBeta, float reflectance, float transmittance);

} // namespace meridian
