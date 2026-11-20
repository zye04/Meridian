#include <doctest/doctest.h>
#include <meridian/farfield.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace meridian;

namespace {

constexpr float kPi = 3.14159265f;

// A 1 m template over [origin, origin + size).
FarBakeGrid bakeGrid(Vec3 origin, uint32_t sx, uint32_t sy, uint32_t sz) {
  FarBakeGrid grid;
  grid.origin = origin;
  grid.size[0] = sx;
  grid.size[1] = sy;
  grid.size[2] = sz;
  const size_t count = size_t{sx} * sy * sz;
  grid.extinction.assign(count, 0.0f);
  grid.normal.assign(count, Vec3{});
  grid.albedo.assign(count, 0);
  return grid;
}

// A crown (extinction falling off from 0.3 per m at its center) on a trunk.
FarBakeGrid treeGrid() {
  FarBakeGrid grid = bakeGrid({-8.0f, 0.0f, -8.0f}, 16, 24, 16);
  for (uint32_t z = 0; z < 16; ++z) {
    for (uint32_t y = 0; y < 24; ++y) {
      for (uint32_t x = 0; x < 16; ++x) {
        const Vec3 p = grid.origin + Vec3{x + 0.5f, y + 0.5f, z + 0.5f};
        const Vec3 fromCrown = p - Vec3{0.0f, 14.0f, 0.0f};
        const size_t i = x + 16 * (y + size_t{24} * z);
        if (length(fromCrown) < 7.0f) {
          grid.extinction[i] = 0.3f * (1.0f - length(fromCrown) / 7.0f) + 0.01f;
          grid.normal[i] = normalize(fromCrown + Vec3{0.0f, 0.1f, 0.0f});
          grid.albedo[i] = 0;
        } else if (std::abs(p.x) < 1.0f && std::abs(p.z) < 1.0f && p.y < 9.0f) {
          grid.extinction[i] = 0.3f;
          grid.normal[i] = {1.0f, 0.0f, 0.0f};
          grid.albedo[i] = 1;
        }
      }
    }
  }
  return grid;
}

const std::vector<Vec3> kPalette = {{0.08f, 0.15f, 0.04f}, {0.3f, 0.25f, 0.2f}};

DecodedInstance instanceAt(Vec3 position, Vec3 axis, float angle, float scale,
                           uint32_t prototype = 0) {
  const float s = std::sin(0.5f * angle);
  const Vec3 a = normalize(axis);
  return {position, {a.x * s, a.y * s, a.z * s, std::cos(0.5f * angle)}, scale, prototype};
}

double templateSum(const FarTemplate &tmpl, float voxelSize) {
  double sum = 0.0;
  for (const FarTemplateVoxel &v : tmpl.voxels) {
    sum += double{v.extinction} * voxelSize * voxelSize * voxelSize;
  }
  return sum;
}

double packedSum(const FarFieldCpu &field, uint32_t level) {
  double sum = 0.0;
  const double volume = std::pow(double{field.voxelSize[level]}, 3.0);
  for (uint32_t i = 0; i < field.brickCount[level] * kBrickVoxels; ++i) {
    sum +=
        unpackVoxel(farBrickVoxel(field.voxels[level].data(), i / kBrickVoxels, i % kBrickVoxels))
            .extinction *
        volume;
  }
  return sum;
}

// Deterministic values in [0, 1).
float hash01(uint32_t i) {
  i = (i ^ 61u) ^ (i >> 16);
  i *= 9u;
  i ^= i >> 4;
  i *= 0x27d4eb2du;
  i ^= i >> 15;
  return static_cast<float>(i >> 8) / 16777216.0f;
}

} // namespace

TEST_CASE("packVoxel round-trips within its quantization") {
  for (float e = 1.0f / 1024.0f; e < 17.0f; e *= 1.013f) {
    const Voxel v = unpackVoxel(packVoxel(e, {0.0f, 1.0f, 0.0f}, 0));
    // Half a code step (2^(1/18)) either way.
    CHECK(std::abs(v.extinction / e - 1.0f) <= 0.0195f);
  }
  CHECK(unpackVoxel(packVoxel(0.0f, {0.0f, 1.0f, 0.0f}, 7)).extinction == 0.0f);
  CHECK(unpackVoxel(packVoxel(1e-6f, {0.0f, 1.0f, 0.0f}, 7)).extinction == 0.0f);
  CHECK(unpackVoxel(packVoxel(100.0f, {0.0f, 1.0f, 0.0f}, 7)).extinction > 16.0f);
  for (uint32_t i = 0; i < 2000; ++i) {
    const float cosTheta = 2.0f * hash01(2 * i) - 1.0f;
    const float phi = 2.0f * kPi * hash01(2 * i + 1);
    const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
    const Vec3 n{sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi)};
    const auto albedo = static_cast<uint8_t>(i % 256);
    const Voxel v = unpackVoxel(packVoxel(0.5f, n, albedo));
    CHECK(dot(v.normal, n) > std::cos(1.5f * kPi / 180.0f));
    CHECK(std::abs(length(v.normal) - 1.0f) < 1e-5f);
    CHECK(v.albedoIndex == albedo);
  }
  for (const Vec3 n : {Vec3{0, 0, 1}, Vec3{0, 0, -1}, Vec3{1, 0, 0}, Vec3{0, -1, 0}}) {
    CHECK(dot(unpackVoxel(packVoxel(1.0f, n, 0)).normal, n) > 0.9999f);
  }
}

TEST_CASE("BrickColumn packs base height and brick count") {
  const BrickColumn column = makeBrickColumn(123456, 65535, 7);
  CHECK(column.firstBrick == 123456);
  CHECK(columnBaseHeight(column) == 65535);
  CHECK(columnBrickCount(column) == 7);
  CHECK(columnBaseHeight(makeBrickColumn(0, 0, 65535)) == 0);
  CHECK(columnBrickCount(makeBrickColumn(0, 0, 65535)) == 65535);

  // Untouched, one brick, a brick and a bit at L0 (16 m = 64 units).
  const uint32_t loHi[] = {UINT32_MAX, 0, 100, 163, 40, 104};
  uint32_t total = 5;
  const std::vector<BrickColumn> columns = farBrickColumns(loHi, 0, total);
  REQUIRE(columns.size() == 3);
  CHECK(columnBrickCount(columns[0]) == 0);
  CHECK(columns[1].firstBrick == 5);
  CHECK(columnBaseHeight(columns[1]) == 100);
  CHECK(columnBrickCount(columns[1]) == 1);
  CHECK(columns[2].firstBrick == 6);
  CHECK(columnBrickCount(columns[2]) == 2);
  CHECK(total == 8);
}

TEST_CASE("A solid box splats to full occupancy, an empty template to none") {
  FarBakeGrid box = bakeGrid({-16.0f, 0.0f, -16.0f}, 32, 32, 32);
  std::fill(box.extinction.begin(), box.extinction.end(), 0.5f);
  std::fill(box.normal.begin(), box.normal.end(), Vec3{0.0f, 1.0f, 0.0f});
  const FarTemplates templates[] = {filterFarTemplate(box),
                                    filterFarTemplate(bakeGrid({-16, 0, -16}, 32, 32, 32))};
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    const uint32_t edge = 32 / static_cast<uint32_t>(kFarVoxelSize[level]);
    CHECK(templates[0][level].voxels.size() == edge * edge * edge);
    CHECK(templates[1][level].voxels.empty());
  }

  const FarFieldGrid grid{0.0f, 0.0f, 0.0f, 4, 4};
  // Template voxel centers on world voxel centers at every level.
  const DecodedInstance solid = instanceAt({112.0f, 96.0f, 80.0f}, {0, 1, 0}, 0.0f, 1.0f, 0);
  const DecodedInstance empty = instanceAt({112.0f, 96.0f, 80.0f}, {0, 1, 0}, 0.0f, 1.0f, 1);
  const std::array<float, kFarLevels> scale = {4096.0f, 4096.0f, 4096.0f};

  const FarFieldCpu field = splatFarField(grid, templates, kPalette, {&solid, 1}, scale);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    const float v = kFarVoxelSize[level];
    uint32_t inside = 0, outside = 0;
    for (float z = 0.5f * v; z < 256.0f; z += v) {
      for (float y = 0.5f * v; y < 256.0f; y += v) {
        for (float x = 0.5f * v; x < 256.0f; x += v) {
          const float e = farFieldExtinction(field, level, {x, y, z});
          const bool inBox = std::abs(x - 112.0f) < 16.0f && y > 96.0f && y < 128.0f &&
                             std::abs(z - 80.0f) < 16.0f;
          if (inBox) {
            inside += std::abs(e / 0.5f - 1.0f) <= 0.0195f ? 1 : 0;
          } else {
            outside += e > 0.0f ? 1 : 0;
          }
        }
      }
    }
    const uint32_t edge = 32 / static_cast<uint32_t>(v);
    CHECK(inside == edge * edge * edge);
    CHECK(outside == 0);
  }

  const FarFieldCpu none = splatFarField(grid, templates, kPalette, {&empty, 1}, scale);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    CHECK(none.voxels[level].empty());
    for (const BrickColumn &column : none.columns[level]) {
      CHECK(columnBrickCount(column) == 0);
    }
  }
}

TEST_CASE("The splat conserves extinction under rotation and scale") {
  const FarTemplates tree = filterFarTemplate(treeGrid());
  const FarFieldGrid grid{-256.0f, -256.0f, 400.0f, 8, 8};
  const std::array<float, kFarLevels> scale = {65536.0f, 65536.0f, 65536.0f};
  struct Case {
    Vec3 axis;
    float angle, scale;
  };
  for (const Case c : {Case{{0, 1, 0}, 0.0f, 1.0f}, Case{{0, 1, 0}, 0.7f, 1.23f},
                       Case{{0.3f, 0.8f, 0.5f}, 1.1f, 0.81f}, Case{{1, 0, 0}, 2.5f, 1.4f}}) {
    const DecodedInstance instance = instanceAt({3.3f, 437.9f, -1.7f}, c.axis, c.angle, c.scale);
    std::array<double, kFarLevels> sums{};
    const FarFieldCpu field =
        splatFarField(grid, {&tree, 1}, kPalette, {&instance, 1}, scale, &sums);
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      const double expected =
          templateSum(tree[level], kFarVoxelSize[level]) * c.scale * c.scale * c.scale;
      CAPTURE(level);
      CAPTURE(c.angle);
      CHECK(expected > 0.0);
      CHECK(std::abs(sums[level] / expected - 1.0) < 0.01);
      // Packing rounds each voxel to its code and drops those under 1/1024
      // per m: the sparse tails of one lone template.
      CHECK(packedSum(field, level) / expected > 0.9);
    }
  }
}

TEST_CASE("Bake pass fractions convert to extinction") {
  CHECK(farExtinctionFromPasses(192, 192) == 0.0f);
  CHECK(farExtinctionFromPasses(96, 192) == doctest::Approx(std::log(2.0f)));
  CHECK(farExtinctionFromPasses(0, 192) == doctest::Approx(std::log(384.0f)));
}

// The production scale: farFixedScale for the game's tree layers (scale
// 0.8-1.3, 117 pattern points per cell, a 3x3-cell reach).
std::array<float, kFarLevels> productionScales(const FarTemplates &tree) {
  std::array<float, kFarLevels> scales{};
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    float maxExtinction = 0.0f;
    for (const FarTemplateVoxel &v : tree[level].voxels) {
      maxExtinction = std::max(maxExtinction, v.extinction);
    }
    scales[level] = farFixedScale(maxExtinction, 0.8f, 1.3f, 1053);
  }
  return scales;
}

TEST_CASE("A dense stand conserves extinction at the production fixed-point scale") {
  const FarTemplates tree = filterFarTemplate(treeGrid());
  const std::array<float, kFarLevels> scales = productionScales(tree);
  // ~5 m spacing over 128 m x 128 m, crowns overlapping.
  std::vector<DecodedInstance> stand;
  std::array<double, kFarLevels> expected{};
  for (uint32_t i = 0; i < 650; ++i) {
    const float s = 0.8f + 0.5f * hash01(4 * i + 2);
    stand.push_back(instanceAt({64.0f + 128.0f * hash01(4 * i), 300.0f + 3.0f * hash01(4 * i + 3),
                                64.0f + 128.0f * hash01(4 * i + 1)},
                               {0, 1, 0}, 6.283f * hash01(4 * i + 3), s));
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      expected[level] += templateSum(tree[level], kFarVoxelSize[level]) * s * s * s;
    }
  }
  const FarFieldGrid grid{0.0f, 0.0f, 250.0f, 4, 4};
  std::array<double, kFarLevels> sums{};
  std::array<int64_t, kFarLevels> largest{};
  splatFarField(grid, {&tree, 1}, kPalette, stand, scales, &sums, &largest);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    CAPTURE(level);
    CHECK(scales[level] > 100.0f);
    CHECK(std::abs(sums[level] / expected[level] - 1.0) < 0.01);
    CHECK(largest[level] < (int64_t{1} << 31));
  }
}

TEST_CASE("The worst-case overlap stays below 2^31 at the production scale") {
  const FarTemplates tree = filterFarTemplate(treeGrid());
  const std::array<float, kFarLevels> scales = productionScales(tree);
  // instancesPerVoxel copies of the largest scale at one point.
  const std::vector<DecodedInstance> stack(
      1053, instanceAt({100.0f, 300.0f, 100.0f}, {0, 1, 0}, 0.0f, 1.3f));
  const FarFieldGrid grid{0.0f, 0.0f, 250.0f, 4, 4};
  std::array<int64_t, kFarLevels> largest{};
  splatFarField(grid, {&tree, 1}, kPalette, stack, scales, nullptr, &largest);
  std::array<float, kFarLevels> tooLarge = scales;
  for (float &scale : tooLarge) {
    scale *= 1000.0f;
  }
  std::array<int64_t, kFarLevels> wrapped{};
  splatFarField(grid, {&tree, 1}, kPalette, stack, tooLarge, nullptr, &wrapped);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    CAPTURE(level);
    CHECK(largest[level] < (int64_t{1} << 31));
    // The check would see an overflow.
    CHECK(wrapped[level] >= (int64_t{1} << 31));
  }
}

TEST_CASE("farFieldTransmittanceCpu matches a brute-force march") {
  const FarTemplates tree = filterFarTemplate(treeGrid());
  // A sparse forest on a slope rising east, 1.3 km across.
  const FarFieldGrid grid{0.0f, 0.0f, 90.0f, 20, 20};
  std::vector<DecodedInstance> instances;
  for (uint32_t i = 0; i < 200; ++i) {
    const float x = 1280.0f * hash01(4 * i), z = 400.0f * hash01(4 * i + 1);
    const float yaw = 2.0f * kPi * hash01(4 * i + 2);
    instances.push_back(
        instanceAt({x, 100.0f + 0.02f * x, z}, {0, 1, 0}, yaw, 0.8f + 0.5f * hash01(4 * i + 3)));
  }
  const std::array<float, kFarLevels> scale = {16384.0f, 16384.0f, 16384.0f};
  const FarFieldCpu field = splatFarField(grid, {&tree, 1}, kPalette, instances, scale);

  const auto bruteForce = [&](Vec3 origin, Vec3 direction, float start, float max) {
    constexpr double kStep = 2.5e-4;
    double depth = 0.0;
    for (double t = start + 0.5 * kStep; t < max; t += kStep) {
      const uint32_t level = t < kFarRayLevelEnd[0] ? 0 : t < kFarRayLevelEnd[1] ? 1 : 2;
      const auto tf = static_cast<float>(t);
      depth += farFieldExtinction(field, level, origin + direction * tf) * kStep;
    }
    return static_cast<float>(std::exp(-depth));
  };
  struct Ray {
    Vec3 origin, direction;
    float start, max;
  };
  const float grazing = 2.0f * kPi / 180.0f;
  const Ray rays[] = {
      // Two degrees down, across the forest and all three levels.
      {{5.0f, 125.0f, 200.0f}, {std::cos(grazing), -std::sin(grazing), 0.0f}, 0.0f, 1270.0f},
      // Along the cell boundary x = 192.
      {{192.0f, 112.0f, 2.0f}, normalize({0.0f, 0.01f, 1.0f}), 0.0f, 390.0f},
      // Along the boundary z = 128, from a voxel surface.
      {{3.0f, 113.0f, 128.0f}, normalize({1.0f, 0.03f, 0.0f}), 4.0f, 1200.0f},
      {{1270.0f, 150.0f, 390.0f}, normalize({-0.9f, -0.04f, -0.31f}), 0.0f, 1250.0f},
      {{600.0f, 135.0f, 10.0f}, normalize({0.3f, -0.02f, 0.5f}), 2.0f, 900.0f},
  };
  for (const Ray &ray : rays) {
    const float expected = bruteForce(ray.origin, ray.direction, ray.start, ray.max);
    const float actual =
        farFieldTransmittanceCpu(field, ray.origin, ray.direction, ray.start, ray.max, nullptr);
    CAPTURE(ray.origin.x);
    CHECK(std::abs(actual - expected) < 1e-3f);
    // Each ray crosses some forest without going dark.
    CHECK(expected < 0.99f);
    CHECK(expected > 0.005f);
  }

  // Cells held by the TLAS are skipped.
  const std::vector<uint8_t> all(size_t{grid.cellsX} * grid.cellsZ, 1);
  CHECK(farFieldTransmittanceCpu(field, rays[0].origin, rays[0].direction, 0.0f, 1270.0f, &all) ==
        1.0f);
}

TEST_CASE("Compact bricks return the dense brick's voxels") {
  // Randomized fill: empty, full, and in between, on both mask halves.
  const uint32_t fills[] = {0, 64, 1, 63, 20, 32, 33, 5, 50, 0, 64, 31};
  std::vector<uint32_t> dense;
  uint32_t seed = 7;
  for (uint32_t fill : fills) {
    const size_t first = dense.size();
    dense.resize(first + kBrickVoxels, 0);
    for (uint32_t placed = 0; placed < fill;) {
      const uint32_t i = static_cast<uint32_t>(hash01(seed++) * kBrickVoxels);
      if (dense[first + i] == 0) {
        dense[first + i] = 1 + static_cast<uint32_t>(hash01(seed++) * 4e9f);
        ++placed;
      }
    }
  }
  const std::vector<uint32_t> pool = compactFarBricks(dense);
  const uint32_t bricks = static_cast<uint32_t>(dense.size() / kBrickVoxels);
  uint32_t occupied = 0;
  for (uint32_t b = 0; b < bricks; ++b) {
    for (uint32_t i = 0; i < kBrickVoxels; ++i) {
      CAPTURE(b);
      CAPTURE(i);
      CHECK(farBrickVoxel(pool.data(), b, i) == dense[size_t{b} * kBrickVoxels + i]);
      occupied += dense[size_t{b} * kBrickVoxels + i] != 0 ? 1 : 0;
    }
  }
  CHECK(pool.size() == size_t{bricks} * kBrickHeaderWords + occupied);
}

TEST_CASE("The sliced splat equals the unsliced splat bit for bit") {
  const FarTemplates tree = filterFarTemplate(treeGrid());
  const std::array<float, kFarLevels> scales = productionScales(tree);
  // Crowns overlapping each other and the cell-row boundaries (z = 64, 128).
  std::vector<DecodedInstance> stand;
  for (uint32_t i = 0; i < 400; ++i) {
    stand.push_back(instanceAt({10.0f + 200.0f * hash01(4 * i), 300.0f + 3.0f * hash01(4 * i + 3),
                                10.0f + 200.0f * hash01(4 * i + 1)},
                               {0, 1, 0}, 6.283f * hash01(4 * i + 3), 0.8f + 0.5f * hash01(i)));
  }
  for (const float z : {63.0f, 64.0f, 127.5f}) {
    stand.push_back(instanceAt({100.0f, 301.0f, z}, {0, 1, 0}, 0.3f, 1.2f));
  }
  const FarFieldGrid grid{0.0f, 0.0f, 250.0f, 4, 4};
  std::array<double, kFarLevels> sums{};
  std::array<int64_t, kFarLevels> largest{};
  const FarFieldCpu whole =
      splatFarField(grid, {&tree, 1}, kPalette, stand, scales, &sums, &largest);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    CHECK(whole.brickCount[level] > 0);
  }
  for (const uint32_t rows : {1u, 2u, 3u}) {
    std::array<double, kFarLevels> slicedSums{};
    std::array<int64_t, kFarLevels> slicedLargest{};
    const FarFieldCpu sliced =
        splatFarField(grid, {&tree, 1}, kPalette, stand, scales, &slicedSums, &slicedLargest, rows);
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      CAPTURE(rows);
      CAPTURE(level);
      CHECK(sliced.voxels[level] == whole.voxels[level]);
      CHECK(sliced.brickCount[level] == whole.brickCount[level]);
      CHECK(slicedSums[level] == sums[level]);
      CHECK(slicedLargest[level] == largest[level]);
      for (size_t c = 0; c < whole.columns[level].size(); ++c) {
        CHECK(sliced.columns[level][c].firstBrick == whole.columns[level][c].firstBrick);
        CHECK(sliced.columns[level][c].heightAndCount == whole.columns[level][c].heightAndCount);
      }
    }
  }
}

TEST_CASE("Slice rows keep each slice within the brick budget") {
  // Level 2: one column per cell; rows of 3 cells with 2, 5, 1, 9 bricks.
  std::vector<BrickColumn> columns;
  for (const uint32_t bricks : {2u, 5u, 1u, 9u}) {
    for (uint32_t x = 0; x < 3; ++x) {
      columns.push_back(makeBrickColumn(0, 0, x == 0 ? bricks : 0));
    }
  }
  CHECK(farSliceRows(columns, 2, 3, 4, 100) == std::vector<uint32_t>{0, 4});
  CHECK(farSliceRows(columns, 2, 3, 4, 7) == std::vector<uint32_t>{0, 2, 3, 4});
  // A row over the budget is its own slice.
  CHECK(farSliceRows(columns, 2, 3, 4, 4) == std::vector<uint32_t>{0, 1, 2, 3, 4});
}
