#pragma once

// Far-field forests (docs/superpowers/specs/2026-11-20-far-field-design.md):
// per 64 m placement cell, sparse columns of 4^3-voxel bricks at three
// levels, splatted at load from the placed table and per-prototype
// templates. This is the CPU side: voxel and column packing, the template
// filter, and CPU replicas of the GPU splat (farfield_splat.comp) and of
// farFieldTransmittance (farfield.glsl).

#include <meridian/math/vec3.h>
#include <meridian/placement.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace meridian {

constexpr uint32_t kFarLevels = 3;
// Voxel edge per level (m). Bricks are 4^3 voxels at every level (16, 32
// and 64 m), so a cell holds 4x4, 2x2 or 1 brick columns.
constexpr float kFarVoxelSize[kFarLevels] = {4.0f, 8.0f, 16.0f};
constexpr uint32_t kBrickEdge = 4;
constexpr uint32_t kBrickVoxels = kBrickEdge * kBrickEdge * kBrickEdge;
constexpr uint32_t farColumnsPerCell(uint32_t level) { return 4u >> level; }
// Global column (x, z) of a level in FarFieldCpu::columns order.
constexpr size_t farColumnIndex(uint32_t cellsX, uint32_t level, uint32_t x, uint32_t z) {
  const uint32_t n = farColumnsPerCell(level);
  return (size_t{z / n} * cellsX + x / n) * n * n + (z % n) * n + x % n;
}
// Column base heights count this unit (m) above FarFieldCpu::heightBase.
constexpr float kFarHeightUnit = 0.25f;
// Rays march L0 for their first 256 m, L1 to 1 km, L2 beyond.
constexpr float kFarRayLevelEnd[kFarLevels - 1] = {256.0f, 1000.0f};
// The prototype bake's template voxel (m).
constexpr float kFarTemplateVoxel = 1.0f;

// A voxel, 4 bytes: extinction code (0 empty; c in 1..255 is
// 2^((c - 1) / 18 - 10) per m, 1/1024 to ~17, steps of 3.9%), octahedral
// normal 2 x u8, albedo palette index. Index in its brick: x + 4 (y + 4 z).
uint32_t packVoxel(float extinction, Vec3 normal, uint8_t albedoIndex);
struct Voxel {
  float extinction;
  Vec3 normal;
  uint8_t albedoIndex;
};
Voxel unpackVoxel(uint32_t voxel);

// One brick column: bricks firstBrick.. (brickCount of them, bottom up) of
// its level's pool, the lowest starting baseHeight kFarHeightUnit above
// heightBase. Must match uvec2 in farfield.glsl.
struct BrickColumn {
  uint32_t firstBrick;
  uint32_t heightAndCount; // baseHeight | brickCount << 16
};
static_assert(sizeof(BrickColumn) == 8);
constexpr BrickColumn makeBrickColumn(uint32_t firstBrick, uint32_t baseHeight,
                                      uint32_t brickCount) {
  return {firstBrick, baseHeight | brickCount << 16};
}
constexpr uint32_t columnBaseHeight(BrickColumn column) { return column.heightAndCount & 0xFFFF; }
constexpr uint32_t columnBrickCount(BrickColumn column) { return column.heightAndCount >> 16; }

// The far field over the placement grid. columns[level]: cell-major (cells
// row-major from the north-west, like the Cell table), then the cell's
// columns row-major. voxels[level]: kBrickVoxels per brick. cellTop and
// cellBottom: the canopy's extent per cell over all levels (bottom > top
// when empty). origin: the grid's north-west corner at heightBase.
struct FarFieldCpu {
  Vec3 origin;
  uint32_t cellsX = 0, cellsZ = 0;
  float cellSize = kCellSize, heightBase = 0.0f;
  float voxelSize[kFarLevels] = {kFarVoxelSize[0], kFarVoxelSize[1], kFarVoxelSize[2]};
  std::vector<BrickColumn> columns[kFarLevels];
  std::vector<uint32_t> voxels[kFarLevels];
  std::vector<float> cellTop, cellBottom;
};

// One occupied voxel of a prototype's template at one level, in prototype
// space (voxel edge kFarVoxelSize[level]). albedo indexes the palette.
// Must match FarTemplateVoxel in farfield.glsl.
struct FarTemplateVoxel {
  Vec3 center;
  float extinction;
  Vec3 normal;
  uint32_t albedo;
};
static_assert(sizeof(FarTemplateVoxel) == 32);
struct FarTemplate {
  std::vector<FarTemplateVoxel> voxels;
  // Bounds of the voxel centers.
  Vec3 centerMin, centerMax;
};
using FarTemplates = std::array<FarTemplate, kFarLevels>;

// A template voxel's extinction (1/m) from how many of its bake rays (each
// kFarTemplateVoxel long) passed; a fully blocked voxel counts half a ray.
float farExtinctionFromPasses(uint32_t passes, uint32_t rays);

// The bake's 1 m template of one prototype, index x + size[0] (y + size[1] z).
struct FarBakeGrid {
  Vec3 origin; // min corner of voxel (0, 0, 0)
  uint32_t size[3] = {};
  std::vector<float> extinction;
  std::vector<Vec3> normal; // unit, or zero where empty
  std::vector<uint8_t> albedo;
};
// Box-filters the 1 m template to each level: mean extinction (which keeps
// the optical mass), extinction-weighted normal, the albedo entry with the
// most extinction.
FarTemplates filterFarTemplate(const FarBakeGrid &grid);

struct FarFieldGrid {
  float originX = 0.0f, originZ = 0.0f, heightBase = 0.0f;
  uint32_t cellsX = 0, cellsZ = 0;
};

// The splat's stages, shared with farfield_splat.comp operation for
// operation. Occupancy: the global columns [x0, x1] x [z0, z1] an instance's
// level template can deposit into (possibly outside the grid) and its
// height range in kFarHeightUnit: every voxel center within half a voxel of
// a deposit, rounded outward.
struct FarFootprint {
  int32_t x0, z0, x1, z1;
  uint32_t lo, hi;
};
FarFootprint farFootprint(const FarFieldGrid &grid, uint32_t level, const FarTemplate &tmpl,
                          const DecodedInstance &instance);
// Allocation: per column (lo, hi) pairs, lo = UINT32_MAX where untouched,
// to columns with consecutive bricks; adds their bricks to brickTotal.
std::vector<BrickColumn> farBrickColumns(std::span<const uint32_t> loHi, uint32_t level,
                                         uint32_t &brickTotal);
// Accumulation is fixed point (order-independent): 7 u32 per voxel
// (extinction, normal xyz, albedo rgb, each times extinction). The scale
// (units per 1/m) keeps the worst case below 2^31: instancesPerVoxel
// instances of scale up to scaleMax, each depositing all of a template
// lattice (spacing scaleMin x voxel) that fits one voxel's support.
constexpr uint32_t kFarChannels = 7;
float farFixedScale(float maxExtinction, float scaleMin, float scaleMax,
                    uint32_t instancesPerVoxel);
// The CPU replica of the splat. templates per prototype (empty: not in the
// far field). If given, per level: extinctionSums, the accumulated
// extinction times voxel volume before packing; largestSums, the largest
// |accumulator| of any channel, summed without wrapping (the GPU's u32 sums
// are only valid below 2^31).
FarFieldCpu splatFarField(const FarFieldGrid &grid, std::span<const FarTemplates> templates,
                          std::span<const Vec3> palette, std::span<const DecodedInstance> instances,
                          const std::array<float, kFarLevels> &fixedScale,
                          std::array<double, kFarLevels> *extinctionSums = nullptr,
                          std::array<int64_t, kFarLevels> *largestSums = nullptr);

// Extinction (1/m) at a world position, at one level; 0 outside the bricks.
float farFieldExtinction(const FarFieldCpu &field, uint32_t level, Vec3 position);
// Transmittance along origin + t direction (unit), t in [start, max], level
// by distance (kFarRayLevelEnd), voxels constant inside. Cells flagged in
// cellInTlas are skipped (M2b; nullptr in M2a).
float farFieldTransmittanceCpu(const FarFieldCpu &field, Vec3 origin, Vec3 direction, float start,
                               float max, const std::vector<uint8_t> *cellInTlas);

} // namespace meridian
