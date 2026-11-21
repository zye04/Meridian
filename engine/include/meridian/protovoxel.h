#pragma once

// Mid tier of the far-field forests (docs/superpowers/specs/2026-11-20-far-field-design.md):
// each tree prototype as a sparse brick hierarchy of voxels in its own
// space, shared by all its instances. Level 0's voxel is the prototype's
// height / kProtoVoxelsPerHeight; levels halve the resolution down to about
// kProtoCoarsestVoxel. Voxels are 8 B: packVoxel (farfield.h) and an
// occlusion word. This is the CPU side: the build from a baked grid, a
// reference march for tests, and the switch rule shared with cull.comp.

#include <meridian/farfield.h>
#include <meridian/math/vec3.h>

#include <cstdint>
#include <vector>

namespace meridian {

constexpr uint32_t kProtoVoxelsPerHeight = 128;
constexpr float kProtoCoarsestVoxel = 4.0f;
constexpr uint32_t kProtoMaxLevels = 8;
constexpr uint32_t kEmptyBrick = 0xFFFFFFFFu;
// A tree draws as voxels once its finest voxel projects to at most this
// many pixels (cull.comp), over a band of kVoxelSwitchBand of that distance
// where both representations draw with complementary dither masks. At 1 px
// the clusters' simplified foliage thinned to bare trunks before the
// switch (~150-260 m); 2 px halves that distance.
constexpr float kVoxelSwitchPixels = 2.0f;
constexpr float kVoxelSwitchBand = 0.1f;
// Voxels draw at the coarsest mip whose voxel projects to at most this many
// pixels: at 1 px the forest overview drew ~4x the voxels for the same
// coverage (the quads are rasterized, so a 2 px voxel is still sharp).
constexpr float kVoxelMipPixels = 2.0f;
// Optical depth at opacity 0.5.
constexpr float kProtoHitOpticalDepth = 0.69314718f;
// The occlusion word's bits 8..31: per axis direction (+x, -x, +y, -y, +z,
// -z) 4 bits of the optical depth from the voxel's center out of the box,
// in steps of this (protoVoxelDepth in protovoxel.glsl blends them).
constexpr float kProtoAxisDepthStep = 0.5f;

// A prototype baked at its finest voxel over [origin, origin + size *
// voxelSize), index x + size[0] (y + size[1] z); every size is a multiple
// of 2^(levels - 1).
struct ProtoBakeGrid {
  Vec3 origin;
  float voxelSize = 1.0f;
  uint32_t size[3] = {};
  std::vector<float> extinction; // 1/m
  std::vector<Vec3> normal;      // unit, or zero where empty
  std::vector<uint8_t> albedo;
};

// One mip. Bricks of 4^3 voxels; brickGrid[gridOffset + x + bricks[0] (y +
// bricks[1] z)] is a brick's index in the pool (from brickOffset) or
// kEmptyBrick. Offsets are relative to the prototype until the renderer
// concatenates the prototypes.
struct ProtoVoxelLevel {
  float voxelSize = 0.0f;
  uint32_t dims[3] = {};
  uint32_t bricks[3] = {};
  uint32_t gridOffset = 0;
  uint32_t brickOffset = 0;
};

struct PrototypeVoxels {
  Vec3 boundsLo, boundsHi;
  std::vector<ProtoVoxelLevel> levels;
  std::vector<uint32_t> brickGrid;
  // Two words per voxel: packVoxel, then the occlusion (0..255, 255 open
  // sky) in the low byte and the axis optical depths above it. Brick b's voxel i (x + 4 (y + 4 z)) is at
  // 2 (64 b + i).
  std::vector<uint32_t> voxels;
  uint32_t brickCount = 0;
};

// Levels for a finest voxel: halve while the next voxel stays within 1.125x
// kProtoCoarsestVoxel.
uint32_t protoLevelCount(float finestVoxel);
// Builds every level from the finest grid, baking the occlusion into it
// (transmittance toward the upper hemisphere through the prototype's own
// extinction).
PrototypeVoxels buildPrototypeVoxels(const ProtoBakeGrid &grid);

// The voxel's packed word and occlusion word at integer voxel (x, y, z) of
// a level; 0 outside or in an empty brick (occlusion 255).
uint32_t protoVoxelAt(const PrototypeVoxels &voxels, uint32_t level, int32_t x, int32_t y,
                      int32_t z, uint32_t *occlusion = nullptr);

struct ProtoHit {
  bool hit = false;
  float t = 0.0f;
  uint32_t voxel = 0;
  uint32_t occlusion = 0;
  // Optical depth accumulated (to the hit, or through the box).
  float opticalDepth = 0.0f;
};
// The first point along origin + t direction (unit, prototype space) where
// the optical depth reaches `limit`, voxels constant inside, t in [0, tMax],
// by fixed small steps (the reference in tests).
ProtoHit marchPrototypeVoxelsBrute(const PrototypeVoxels &voxels, uint32_t level, Vec3 origin,
                                   Vec3 direction, float tMax, float limit);

// The fraction of axis-aligned rays through a level's box (along x, y, z)
// its voxels block: the mean of 1 - exp(-optical depth) over the columns.
Vec3 protoCoverage(const PrototypeVoxels &voxels, uint32_t level);

// The optical depth from a voxel's center out of the box along unit d, from
// its occlusion word (protoVoxelDepth in protovoxel.glsl).
float protoVoxelDepth(uint32_t occlusionWord, Vec3 d);

// The switch rule (cull.comp). pixelsPerMeter: the pixels a meter spans at
// distance 1 (|projectionScale.y| * 0.5 / inverseResolution.y); scale: the
// instance's.
float voxelSwitchDistance(float finestVoxel, float scale, float pixelsPerMeter);
// Weight of the voxel representation at a distance: 0 up to the switch, 1
// from the switch distance times (1 + band); the clusters draw with 1 minus
// it, per pixel against the same noise.
float voxelBlend(float distance, float switchDistance);
// The coarsest level whose voxel projects to at most kVoxelMipPixels.
uint32_t voxelMip(float distance, float finestVoxel, float scale, float pixelsPerMeter,
                  uint32_t levelCount);

} // namespace meridian
