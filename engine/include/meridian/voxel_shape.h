#pragma once

// How a voxel cluster's voxel draws (voxel_lod.glsl, voxel_instance.mesh):
// the CPU mirror the tests check.

#include <meridian/math/vec3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

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

// Development rules for a voxel of coverage f in the main view
// (--voxel-coverage; spec 2026-11-26-voxel-leaf-coverage-design.md): the
// quad drawn at area fraction a with chance p, a p = f. Hash: a = 1, p = f
// (voxelDraws, the floor included). Area: a = f, p = 1. AreaOffset: as Area,
// the quad moved inside the full quad by the voxel's hash. Mixed: a = p =
// sqrt(f). Phase: as Hash, but a quad under one pixel draws on round(P f) of
// the P jitter phases, spread evenly. AreaPixel: a = f, but never under one
// pixel (a = 1 / q for a full quad of q pixels, at most 1), p = f / a by the
// hash. AreaPixelPhase: as AreaPixel, p from the jitter-phase sequence.
// Cascades keep Hash. Must match voxel_lod.glsl.
enum class VoxelCoverage : uint32_t { Hash, Area, AreaOffset, Mixed, Phase, AreaPixel, AreaPixelPhase };

// `pixels`: the full quad's area on screen (q).
inline float voxelAreaFraction(VoxelCoverage rule, float f, float pixels) {
  switch (rule) {
  case VoxelCoverage::Area:
  case VoxelCoverage::AreaOffset:
    return f;
  case VoxelCoverage::Mixed:
    return std::sqrt(f);
  case VoxelCoverage::AreaPixel:
  case VoxelCoverage::AreaPixelPhase:
    return pixels > 0.0f ? std::min(std::max(f, 1.0f / pixels), 1.0f) : 1.0f;
  default:
    return 1.0f;
  }
}

// Whether the phase sequence draws on jitter phase `phase` of `phases` a
// voxel of coverage f, its sequence shifted by `offset` (its hash): round(P
// f) draws per cycle, at most one phase apart from even.
inline bool voxelPhaseDraws(float f, uint32_t phase, uint32_t phases, uint32_t offset) {
  const uint32_t draws = static_cast<uint32_t>(std::lround(static_cast<float>(phases) * f));
  const uint32_t j = (phase + offset) % phases;
  return (j + 1) * draws / phases > j * draws / phases;
}

// A quad under this many pixels of area takes Phase's sequence: one pixel,
// where the jitter moves the quad across pixel centres.
constexpr float kPhaseVoxelPixels = 1.0f;

// Whether a voxel of coverage f draws: noise uniform in [0, 1) from its
// stable hash, pixels its full quad's area on screen.
inline bool voxelCoverageDraws(VoxelCoverage rule, float f, bool opaque, float noise, float pixels,
                               uint32_t phase, uint32_t phases, uint32_t offset) {
  switch (rule) {
  case VoxelCoverage::Hash:
    return voxelDraws(f, opaque, noise);
  case VoxelCoverage::Area:
  case VoxelCoverage::AreaOffset:
    return f > 0.0f;
  case VoxelCoverage::Mixed:
    return noise < std::sqrt(f);
  case VoxelCoverage::Phase:
    return pixels < kPhaseVoxelPixels ? voxelPhaseDraws(f, phase, phases, offset) : noise < f;
  case VoxelCoverage::AreaPixel:
    return noise < f / voxelAreaFraction(rule, f, pixels);
  case VoxelCoverage::AreaPixelPhase:
    return voxelPhaseDraws(f / voxelAreaFraction(rule, f, pixels), phase, phases, offset);
  }
  return false;
}

// AreaOffset: the scaled quad's centre along one screen axis, in half sizes
// of the full quad, from u uniform in [0, 1): anywhere that keeps the quad
// inside the full one. 0 for the other rules.
inline float voxelQuadOffset(VoxelCoverage rule, float f, float u) {
  if (rule != VoxelCoverage::AreaOffset) {
    return 0.0f;
  }
  return (2.0f * u - 1.0f) * (1.0f - std::sqrt(f));
}

} // namespace meridian
