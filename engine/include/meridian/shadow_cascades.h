#pragma once

#include <meridian/math/mat4.h>
#include <meridian/math/vec3.h>

#include <array>
#include <cstdint>

namespace meridian {

constexpr uint32_t kShadowCascades = 5;
constexpr uint32_t kShadowMapSize = 2048;
// Must match kShadowRayLength in common.glsl.
constexpr double kShadowRayLength = 20000.0;

struct DVec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

inline DVec3 operator+(DVec3 a, DVec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline DVec3 operator-(DVec3 a, DVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline DVec3 operator*(DVec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(DVec3 a, DVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Orthonormal, right-handed (u x v = d): d points away from
// the sun, u and v span the shadow map's plane.
struct LightBasis {
  DVec3 u, v, d;
};

LightBasis lightBasis(Vec3 sunDirection);

// Cascade i covers receivers within cascadeRadius(i) of the camera; its
// window is cascadeHalfSize(i) wide, the extra being filter margin.
double cascadeRadius(uint32_t i);
double cascadeHalfSize(uint32_t i);

struct CascadeWindow {
  DVec3 center;
  double halfSize = 0.0;
  double texel = 0.0;
  // Distances from center along the sun direction where the box starts
  // (toward the sun) and ends (away from it).
  double depthNear = 0.0;
  double depthFar = 0.0;
};

// The window is centred on the camera's (u, v) snapped down to whole texels,
// so a re-centred cascade samples the same texel grid and does not shimmer.
// casterTop is the highest world y that can cast a shadow.
CascadeWindow cascadeWindow(uint32_t i, const LightBasis &basis, DVec3 camera,
                            double casterTop, Vec3 sunDirection);

// Maps world - center to clip: x, y in [-1, 1] over +-halfSize along u, v;
// reverse depth, 1 at depthNear and 0 at depthFar.
Mat4 cascadeProjection(const CascadeWindow &window, const LightBasis &basis);

// Inward planes relative to center, as SceneData::frustumPlanes (inside where
// dot(n, p) + w >= 0). Order: -u, +u, -v, +v, sunward, far.
std::array<std::array<float, 4>, 6> cascadePlanes(const CascadeWindow &window,
                                                  const LightBasis &basis);

bool needsRecenter(uint32_t i, const CascadeWindow &rendered, const LightBasis &basis,
                   DVec3 camera);

} // namespace meridian
