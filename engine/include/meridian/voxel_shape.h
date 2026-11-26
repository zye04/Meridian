#pragma once

// How a voxel cluster's voxel draws (voxel_lod.glsl, voxel_instance.mesh):
// the CPU mirror the tests check.

#include <meridian/math/vec3.h>

#include <cmath>

namespace meridian {

// A cube of edge `edge` seen along unit `d` covers edge^2 (|dx| + |dy| +
// |dz|) on screen: between edge^2 (along an axis) and sqrt(3) edge^2 (along
// a diagonal).
inline float voxelQuadArea(Vec3 d, float edge) {
  return edge * edge * (std::abs(d.x) + std::abs(d.y) + std::abs(d.z));
}

// Opacity along unit d of a voxel with optical depths `depth` along x, y, z:
// its mean chord is edge / S, its extinction along d blends the axes' by d^2.
inline float voxelViewOpacity(const float depth[3], Vec3 d) {
  const float s = std::abs(d.x) + std::abs(d.y) + std::abs(d.z);
  return 1.0f - std::exp(-(d.x * d.x * depth[0] + d.y * d.y * depth[1] + d.z * d.z * depth[2]) / s);
}

// Whether the coverage hash (uniform `noise` in [0, 1)) draws a voxel of
// that opacity along the view; `opaque`: no transmission (wood). Drawn or
// dropped whole, a voxel of coverage f errs by 2 f (1 - f) of its quad on
// average through the hash and by 1 - f drawn always, less from f = 1/2: an
// opaque part's partly covered cells (silhouette, base) then draw instead of
// opening holes a pixel or two wide (voxels draw at that size). Must match
// voxel_lod.glsl.
constexpr float kOpaqueVoxelFloor = 0.5f;
inline bool voxelDraws(float opacity, bool opaque, float noise) {
  return noise < opacity || (opaque && opacity >= kOpaqueVoxelFloor);
}

} // namespace meridian
