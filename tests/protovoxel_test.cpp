#include <doctest/doctest.h>
#include <meridian/protovoxel.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace meridian;

namespace {

ProtoBakeGrid emptyGrid(uint32_t sx, uint32_t sy, uint32_t sz, float voxel) {
  ProtoBakeGrid grid;
  grid.origin = {0.0f, 0.0f, 0.0f};
  grid.voxelSize = voxel;
  grid.size[0] = sx;
  grid.size[1] = sy;
  grid.size[2] = sz;
  const size_t count = size_t{sx} * sy * sz;
  grid.extinction.assign(count, 0.0f);
  grid.normal.assign(count, Vec3{});
  grid.albedo.assign(count, 0);
  return grid;
}

void fill(ProtoBakeGrid &grid, uint32_t x0, uint32_t y0, uint32_t z0, uint32_t x1, uint32_t y1,
          uint32_t z1, float extinction) {
  for (uint32_t z = z0; z < z1; ++z) {
    for (uint32_t y = y0; y < y1; ++y) {
      for (uint32_t x = x0; x < x1; ++x) {
        const size_t i = x + size_t{grid.size[0]} * (y + size_t{grid.size[1]} * z);
        grid.extinction[i] = extinction;
        grid.normal[i] = {0.0f, 1.0f, 0.0f};
      }
    }
  }
}

double mass(const PrototypeVoxels &voxels, uint32_t level) {
  const ProtoVoxelLevel &l = voxels.levels[level];
  double sum = 0.0;
  for (uint32_t z = 0; z < l.bricks[2] * 4; ++z) {
    for (uint32_t y = 0; y < l.bricks[1] * 4; ++y) {
      for (uint32_t x = 0; x < l.bricks[0] * 4; ++x) {
        sum += unpackVoxel(protoVoxelAt(voxels, level, x, y, z)).extinction;
      }
    }
  }
  return sum * std::pow(l.voxelSize, 3.0);
}

} // namespace

TEST_CASE("A solid box voxelizes to full occupancy, an empty prototype to none") {
  ProtoBakeGrid grid = emptyGrid(32, 32, 32, 0.5f);
  fill(grid, 0, 0, 0, 32, 32, 32, 1.5f);
  const PrototypeVoxels solid = buildPrototypeVoxels(grid);
  CHECK(solid.levels.size() == protoLevelCount(0.5f));
  for (uint32_t level = 0; level < solid.levels.size(); ++level) {
    const ProtoVoxelLevel &l = solid.levels[level];
    for (uint32_t z = 0; z < l.dims[2]; ++z) {
      for (uint32_t y = 0; y < l.dims[1]; ++y) {
        for (uint32_t x = 0; x < l.dims[0]; ++x) {
          REQUIRE(protoVoxelAt(solid, level, x, y, z) != 0);
        }
      }
    }
  }
  const PrototypeVoxels empty = buildPrototypeVoxels(emptyGrid(32, 32, 32, 0.5f));
  CHECK(empty.brickCount == 0);
  CHECK(empty.voxels.empty());
  CHECK(protoVoxelAt(empty, 0, 3, 3, 3) == 0);
}

TEST_CASE("Mips conserve extinction-weighted mass") {
  ProtoBakeGrid grid = emptyGrid(64, 64, 64, 0.25f);
  fill(grid, 5, 3, 7, 40, 50, 33, 2.0f);
  fill(grid, 20, 20, 20, 30, 60, 30, 0.7f);
  const PrototypeVoxels voxels = buildPrototypeVoxels(grid);
  REQUIRE(voxels.levels.size() >= 3);
  const double base = mass(voxels, 0);
  for (uint32_t level = 1; level < voxels.levels.size(); ++level) {
    CHECK(mass(voxels, level) == doctest::Approx(base).epsilon(0.01));
  }
}

TEST_CASE("A voxel deep in a solid block is darker than one on its top face") {
  ProtoBakeGrid grid = emptyGrid(32, 32, 32, 0.5f);
  fill(grid, 4, 4, 4, 28, 28, 28, 3.0f);
  const PrototypeVoxels voxels = buildPrototypeVoxels(grid);
  uint32_t deep = 0, top = 0;
  protoVoxelAt(voxels, 0, 16, 10, 16, &deep);
  protoVoxelAt(voxels, 0, 16, 27, 16, &top);
  deep &= 0xFF;
  top &= 0xFF;
  CHECK(deep < top);
  CHECK(top > 200);
  CHECK(deep < 20);
}

TEST_CASE("Baked axis depths match a march from the voxel's center") {
  ProtoBakeGrid grid = emptyGrid(64, 64, 64, 0.25f);
  fill(grid, 10, 0, 10, 20, 30, 20, 1.2f);
  fill(grid, 30, 20, 25, 52, 45, 50, 0.4f);
  fill(grid, 8, 40, 8, 60, 44, 60, 2.5f);
  const PrototypeVoxels voxels = buildPrototypeVoxels(grid);
  const Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  uint32_t compared = 0;
  for (uint32_t level = 0; level < voxels.levels.size(); ++level) {
    const float v = voxels.levels[level].voxelSize;
    const uint32_t n = 64 >> level;
    for (uint32_t z = 0; z < n; z += 3) {
      for (uint32_t y = 0; y < n; y += 3) {
        for (uint32_t x = 0; x < n; x += 3) {
          uint32_t word = 0;
          if (protoVoxelAt(voxels, level, x, y, z, &word) == 0) {
            continue;
          }
          const Vec3 center = Vec3{x + 0.5f, y + 0.5f, z + 0.5f} * v;
          for (const Vec3 &axis : axes) {
            const float marched =
                marchPrototypeVoxelsBrute(voxels, level, center, axis, 100.0f, 1e30f).opticalDepth;
            // Quantized to half a step, the march's steps add a little.
            CHECK(std::abs(protoVoxelDepth(word, axis) -
                           std::min(marched, 15.0f * kProtoAxisDepthStep)) <=
                  0.5f * kProtoAxisDepthStep + 0.05f);
            ++compared;
          }
        }
      }
    }
  }
  CHECK(compared > 600);
}

TEST_CASE("The mid switch: band weights complementary, distance monotonic in height") {
  const float pixelsPerMeter = 1000.0f;
  float previous = 0.0f;
  for (float height : {5.0f, 10.0f, 25.0f, 60.0f}) {
    const float d = voxelSwitchDistance(height / kProtoVoxelsPerHeight, 1.0f, pixelsPerMeter);
    CHECK(d > previous);
    previous = d;
  }
  const float d = voxelSwitchDistance(25.0f / kProtoVoxelsPerHeight, 1.0f, pixelsPerMeter);
  CHECK(voxelBlend(d * 0.5f, d) == 0.0f);
  CHECK(voxelBlend(d * 1.05f, d) == doctest::Approx(0.5f));
  CHECK(voxelBlend(d * 1.2f, d) == 1.0f);
  for (float f = 0.9f; f < 1.2f; f += 0.01f) {
    const float w = voxelBlend(d * f, d);
    CHECK(w + (1.0f - w) == doctest::Approx(1.0f));
    CHECK(w >= 0.0f);
    CHECK(w <= 1.0f);
  }
  // At the switch the finest voxel is kVoxelSwitchPixels (2 px): mip 0.
  CHECK(voxelMip(d, 25.0f / 128.0f, 1.0f, pixelsPerMeter, 5) == 0);
  CHECK(voxelMip(d * 2.0f, 25.0f / 128.0f, 1.0f, pixelsPerMeter, 5) == 1);
  CHECK(voxelMip(d * 4.5f, 25.0f / 128.0f, 1.0f, pixelsPerMeter, 5) == 2);
  CHECK(voxelMip(d * 1000.0f, 25.0f / 128.0f, 1.0f, pixelsPerMeter, 5) == 4);
}
