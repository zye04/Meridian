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

// How the main view draws a voxel of coverage f (spec
// 2026-11-26-voxel-leaf-coverage-design.md): a quad of area fraction a with
// chance p, a p = f. AreaPixelPhase (the default): a leaf voxel's quad
// shrinks to a = f but never under one pixel (a = 1 / q for a full quad of q
// pixels, at most 1), and draws on round(P f / a) of the P jitter phases,
// spread evenly, so a still camera converges to f over one cycle. Wood keeps
// Hash (a = 1, voxelDraws with its floor): centre-shrunk quads turn a trunk
// one or two voxels wide into a lattice of dots. Hash for every voxel is
// kept for A/B (--voxel-coverage hash); the cascades always take Hash. Must
// match voxel_lod.glsl.
enum class VoxelCoverage : uint32_t { Hash, AreaPixelPhase };

// Whether the voxel draws its whole quad by the hash.
inline bool voxelDrawsWhole(VoxelCoverage rule, bool opaque) {
  return rule == VoxelCoverage::Hash || opaque;
}

// `pixels`: the full quad's area on screen (q).
inline float voxelAreaFraction(float f, float pixels) {
  return pixels > 0.0f ? std::min(std::max(f, 1.0f / pixels), 1.0f) : 1.0f;
}

// Whether the phase sequence draws on jitter phase `phase` of `phases` a
// voxel of draw chance p, its sequence shifted by `offset` (its hash):
// round(P p) draws per cycle, at most one phase apart from even.
inline bool voxelPhaseDraws(float p, uint32_t phase, uint32_t phases, uint32_t offset) {
  const uint32_t draws = static_cast<uint32_t>(std::lround(static_cast<float>(phases) * p));
  const uint32_t j = (phase + offset) % phases;
  return (j + 1) * draws / phases > j * draws / phases;
}

// Whether a voxel of coverage f draws: noise uniform in [0, 1) from its
// stable hash, pixels its full quad's area on screen.
inline bool voxelCoverageDraws(VoxelCoverage rule, float f, bool opaque, float noise, float pixels,
                               uint32_t phase, uint32_t phases, uint32_t offset) {
  return voxelDrawsWhole(rule, opaque) ? voxelDraws(f, opaque, noise)
                                       : voxelPhaseDraws(f / voxelAreaFraction(f, pixels), phase, phases, offset);
}

} // namespace meridian
