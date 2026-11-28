#pragma once

// Far-field forests (docs/superpowers/archive/specs/2026-11-20-far-field-design.md):
// per 64 m placement cell, sparse columns of 4^3-voxel bricks at three
// levels, splatted at load from the placed table and per-prototype
// templates. This is the CPU side: voxel and column packing, the template
// filter, and CPU replicas of the GPU splat (farfield_splat.comp) and of
// farFieldTransmittance (farfield.glsl).

#include <meridian/math/vec3.h>
#include <meridian/placement.h>
#include <meridian/voxelize.h>

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
// The prototypes' template voxel (m): their meshes voxelized at this edge
// by the cluster build (ClusterLod::farBricks).
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

// A level's bricks are compact: one pool of u32, first a BrickHeader per
// brick (kBrickHeaderWords each, in brick order), then the occupied (non-zero)
// voxels of every brick in index order. maskLo / maskHi: bit i of the 64
// voxel indices is set where the voxel is occupied; firstVoxel: the pool
// index of the brick's first occupied voxel. Voxel i of brick b is
// pool[firstVoxel + bitCount(mask & ((1 << i) - 1))] if its bit is set, else
// 0 (farBrickVoxel). Must match the pool reads in farfield.glsl.
struct BrickHeader {
  uint32_t maskLo, maskHi, firstVoxel;
};
static_assert(sizeof(BrickHeader) == 12);
constexpr uint32_t kBrickHeaderWords = 3;
uint32_t farBrickVoxel(const uint32_t *pool, uint32_t brick, uint32_t local);
// The pool of dense bricks (kBrickVoxels voxels each).
std::vector<uint32_t> compactFarBricks(std::span<const uint32_t> dense);

// The far field over the placement grid. columns[level]: cell-major (cells
// row-major from the north-west, like the Cell table), then the cell's
// columns row-major. voxels[level]: the level's brick pool, brickCount[level]
// bricks. cellTop and cellBottom: the canopy's extent per cell over all
// levels (bottom > top when empty). origin: the grid's north-west corner at
// heightBase.
struct FarFieldCpu {
  Vec3 origin;
  uint32_t cellsX = 0, cellsZ = 0;
  float cellSize = kCellSize, heightBase = 0.0f;
  float voxelSize[kFarLevels] = {kFarVoxelSize[0], kFarVoxelSize[1], kFarVoxelSize[2]};
  std::vector<BrickColumn> columns[kFarLevels];
  std::vector<uint32_t> voxels[kFarLevels];
  uint32_t brickCount[kFarLevels] = {};
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

// The 1 m template of one prototype, index x + size[0] (y + size[1] z).
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

// The clumping index of the far field's uniform-medium boxes (Nilson 1971):
// the Omega for which a box's transmittance exp(-Omega tau) matches, summed
// over rays through the bakes, the transmittance of their 1 m voxels. Rays
// run at `zenith` radians from vertical (eight azimuths unless vertical), one
// per metre across each grid, through boxes `boxSize` m on the grid's
// lattice (the level filter's). 1 where no ray meets foliage.
float farClumpingIndex(std::span<const FarBakeGrid> grids, float boxSize, float zenith);
// The zenith angles (radians) the renderer measures it at: 0, 45 and 75 degrees.
inline constexpr std::array<float, 3> kFarClumpingZeniths = {0.0f, 0.785398f, 1.308997f};

// Per palette slot: coverage-weighted sums of its voxels' reflected and
// transmitted colours (VoxelPayload::albedo, ::transmission) and the
// coverage, which the palette entry divides by.
struct FarSlotColor {
  double coverage = 0.0;
  Vec3 albedo, transmission;
};
// A prototype's 1 m template from its meshes' voxels (slots[i]: mesh i's
// triangles voxelized at kFarTemplateVoxel on the shared lattice, so the
// grid's origin is a lattice corner). Coincident voxels combine as
// independent media (combineVoxelGrids); extinction is the mean optical
// depth over the three axes per metre, the normal the combined lobe's axis
// (sign free: the far shading faces it to the viewer), the albedo
// paletteBase + the slot with the most coverage there. Adds each slot's
// colour sums to colors[slot].
FarBakeGrid farBakeGridFromVoxels(std::span<const VoxelGrid> slots, uint8_t paletteBase,
                                  std::span<FarSlotColor> colors);

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
// The splat runs in slices of placement cell rows [rows[i], rows[i + 1]) so
// one slice's accumulators (kBrickVoxels x kFarChannels u32 per brick) stay
// within maxBricks bricks (a single row over it is its own slice). Returns
// the boundaries, 0 and columns.size() / columns per row at the ends.
std::vector<uint32_t> farSliceRows(std::span<const BrickColumn> columns, uint32_t level,
                                   uint32_t cellsX, uint32_t cellsZ, uint64_t maxBricks);
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
// are only valid below 2^31). sliceRows: cell rows per slice (0: one
// slice); the result is identical for every value.
FarFieldCpu splatFarField(const FarFieldGrid &grid, std::span<const FarTemplates> templates,
                          std::span<const Vec3> palette, std::span<const DecodedInstance> instances,
                          const std::array<float, kFarLevels> &fixedScale,
                          std::array<double, kFarLevels> *extinctionSums = nullptr,
                          std::array<int64_t, kFarLevels> *largestSums = nullptr,
                          uint32_t sliceRows = 0);

// Extinction (1/m) at a world position, at one level; 0 outside the bricks.
float farFieldExtinction(const FarFieldCpu &field, uint32_t level, Vec3 position);
// Transmittance along origin + t direction (unit), t in [start, max], level
// by distance (kFarRayLevelEnd), voxels constant inside. Cells flagged in
// cellInTlas are skipped (M2b; nullptr in M2a).
float farFieldTransmittanceCpu(const FarFieldCpu &field, Vec3 origin, Vec3 direction, float start,
                               float max, const std::vector<uint8_t> *cellInTlas);

// ------------------------------------------------------------ Sun shadow map
// An orthographic depth map along the sun over the forest bounds, replacing
// the per-pixel farFieldTransmittance march of far receivers: per texel the
// distance (from the map's near plane) of the first point where the opacity
// of the bricks along the sun reaches 0.5, 16-bit (kFarShadowEmpty: none).
// Must match farfield.glsl.
constexpr uint32_t kFarShadowLevel = 1;
constexpr float kFarShadowOpticalDepth = 0.6931472f; // -ln(1 - 0.5)
constexpr float kFarShadowTexel = 8.0f;
constexpr uint32_t kFarShadowEmpty = 0xFFFF;
// A texel whose march ran out of steps (a bug if any; treated as lit).
constexpr uint32_t kFarShadowStepCap = 0xFFFE;
// The map is rebuilt in a second buffer when the sun turned more than the
// first angle from the build direction; until the swap it serves while the
// sun stays within the second, else receivers march the bricks per ray.
constexpr float kShadowMapRebuildAngle = 0.1f * 3.14159265f / 180.0f;
constexpr float kShadowMapStaleAngle = 1.0f * 3.14159265f / 180.0f;
// A build under way restarts toward the newest sun only once the sun is this
// far from the direction it builds; closer, it finishes (the spec's 0.1
// degrees starves under a slow drag: a rebuild takes ~80 frames) and the
// next one starts toward the then-current sun.
constexpr float kShadowMapRestartAngle = kShadowMapStaleAngle;
// Texels built per frame during a rebuild (even: two texels per word), about
// 0.3 ms at 1440p Release.
constexpr uint32_t kFarShadowTexelBudget = 131072;

struct FarShadowBox {
  Vec3 lo, hi; // world
};
// The map's frame: u, v, d orthonormal, d the direction light travels (away
// from the sun). A texel (x, y) is the ray from (minU + (x + 0.5) texel) u +
// (minV + (y + 0.5) texel) v + minD d along d, for `depth` meters.
struct FarShadowGrid {
  Vec3 u, v, d;
  float minU = 0.0f, minV = 0.0f, minD = 0.0f, texel = 0.0f, depth = 0.0f;
  uint32_t width = 0, height = 0;
};
// The grid of `box` for the sun at unit `toSun`, texels doubled from
// kFarShadowTexel until at most maxTexels.
FarShadowGrid farShadowGrid(const FarShadowBox &box, Vec3 toSun, uint32_t maxTexels);
// width * height rounded up to even.
constexpr uint32_t farShadowTexelCount(const FarShadowGrid &grid) {
  return (grid.width * grid.height + 1) & ~1u;
}
// The most texels farShadowGrid(box, any sun) needs (sampled over the sky).
uint32_t farShadowMaxTexels(const FarShadowBox &box);
uint32_t quantizeFarShadow(float distance, float depth);
float dequantizeFarShadow(uint32_t value, float depth);

// Where the opacity of the level's voxels along origin + t direction
// reaches kFarShadowOpticalDepth (t in [0, max]), or -1 (CPU twin of
// farFieldShadowDistance in farfield.glsl).
float farFieldShadowDistanceCpu(const FarFieldCpu &field, uint32_t level, Vec3 origin,
                                Vec3 direction, float max);

// What the renderer does each frame about the map (pure, for tests).
struct FarShadowState {
  bool hasBuilt = false;
  Vec3 builtDir; // toward the sun
  bool building = false;
  Vec3 buildingDir;
  uint32_t progress = 0, buildingTotal = 0; // texels
};
struct FarShadowStep {
  FarShadowState state;
  // Whether receivers read the built map this frame (else the per-ray march).
  bool useMap = false;
  // The texels [first, first + count) to build this frame into the second
  // map (count 0: none).
  uint32_t first = 0, count = 0;
  // The slice ends the build: the maps swap after this frame.
  bool completes = false;
};
// sunTexels: the texel count of the map for `sun` (used when a build starts
// or restarts). budget: texels per frame (even).
FarShadowStep farShadowStep(const FarShadowState &state, Vec3 sun, uint32_t sunTexels,
                            uint32_t budget);

} // namespace meridian
