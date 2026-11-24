#include <doctest/doctest.h>
#include <meridian/voxelize.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

using namespace meridian;

namespace {

float &at(Vec3 &v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

VoxelTriangle tri(Vec3 a, Vec3 b, Vec3 c, uint32_t material = 0) {
  VoxelTriangle t{};
  t.p[0] = a;
  t.p[1] = b;
  t.p[2] = c;
  const Vec3 n = normalize(cross(b - a, c - a));
  for (int v = 0; v < 3; ++v) {
    t.n[v] = n;
  }
  t.uv[0][0] = 0, t.uv[0][1] = 0;
  t.uv[1][0] = 1, t.uv[1][1] = 0;
  t.uv[2][0] = 1, t.uv[2][1] = 1;
  t.material = material;
  return t;
}

// Two triangles covering [lo, lo + size] in the plane normal to `axis`, at
// lo[axis]; uv 0..1 across the first tangent axis.
void quad(std::vector<VoxelTriangle> &out, Vec3 lo, float size, int axis, uint32_t material = 0) {
  const int u = (axis + 1) % 3, w = (axis + 2) % 3;
  auto corner = [&](float a, float b) {
    Vec3 p = lo;
    at(p, u) += a * size;
    at(p, w) += b * size;
    return p;
  };
  VoxelTriangle t0 = tri(corner(0, 0), corner(1, 0), corner(1, 1), material);
  VoxelTriangle t1 = tri(corner(0, 0), corner(1, 1), corner(0, 1), material);
  t0.uv[0][0] = 0, t0.uv[1][0] = 1, t0.uv[2][0] = 1;
  t1.uv[0][0] = 0, t1.uv[1][0] = 1, t1.uv[2][0] = 0;
  out.push_back(t0);
  out.push_back(t1);
}

std::vector<VoxelTriangle> leafSoup(uint32_t count, float extent, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> pos(0.0f, extent), dir(-1.0f, 1.0f), size(0.05f, 0.2f);
  std::vector<VoxelTriangle> out;
  for (uint32_t i = 0; i < count; ++i) {
    const Vec3 c{pos(rng), pos(rng), pos(rng)};
    const Vec3 a = normalize(Vec3{dir(rng), dir(rng), dir(rng)} + Vec3{0.0f, 0.0f, 1e-3f});
    Vec3 b = normalize(cross(a, Vec3{0.31f, 0.53f, 0.79f}));
    const Vec3 d = cross(a, b);
    const float s = size(rng);
    out.push_back(tri(c, c + b * s, c + (b + d) * s));
    out.push_back(tri(c, c + (b + d) * s, c + d * s));
  }
  return out;
}

const VoxelPayload *voxelAt(const VoxelGrid &grid, int32_t x, int32_t y, int32_t z) {
  for (const VoxelBrick &brick : grid.bricks) {
    const int32_t lx = x - brick.origin[0], ly = y - brick.origin[1], lz = z - brick.origin[2];
    if (lx < 0 || ly < 0 || lz < 0 || lx >= 4 || ly >= 4 || lz >= 4) {
      continue;
    }
    const uint32_t i = lx + 4 * (ly + 4 * lz);
    return (brick.occupied >> i) & 1u ? &brick.voxels[i] : nullptr;
  }
  return nullptr;
}

const float kOpaque = -std::log(1.0f / 32.0f);

} // namespace

TEST_CASE("a closed cube's face voxels are opaque along the face normal") {
  std::vector<VoxelTriangle> tris;
  for (int axis = 0; axis < 3; ++axis) {
    quad(tris, {0.0f, 0.0f, 0.0f}, 1.0f, axis);
    Vec3 far{0.0f, 0.0f, 0.0f};
    at(far, axis) = 0.999f;
    quad(tris, far, 1.0f, axis);
  }
  const VoxelMaterial material{};
  const VoxelGrid grid = voxelize(tris, {&material, 1}, 0.25f);
  // Voxels on the x = 0.999 face (x index 3), away from the edges.
  for (int32_t y = 1; y < 3; ++y) {
    for (int32_t z = 1; z < 3; ++z) {
      const VoxelPayload *v = voxelAt(grid, 3, y, z);
      REQUIRE(v != nullptr);
      CHECK(v->depth[0] == doctest::Approx(kOpaque).epsilon(1e-4));
      CHECK(v->depth[1] == doctest::Approx(0.0f));
    }
  }
  CHECK(voxelAt(grid, 1, 1, 1) == nullptr); // a surface voxelization: the inside is empty
}

TEST_CASE("voxel optical depth matches brute-force rays at the same positions") {
  const std::vector<VoxelTriangle> tris = leafSoup(200, 2.0f, 1);
  const VoxelMaterial material{};
  const VoxelGrid grid = voxelize(tris, {&material, 1}, 0.5f);
  size_t checked = 0;
  for (const VoxelBrick &brick : grid.bricks) {
    for (uint32_t i = 0; i < 64; ++i) {
      if (!((brick.occupied >> i) & 1u)) {
        continue;
      }
      const Vec3 lo{(brick.origin[0] + int32_t(i % 4)) * 0.5f,
                    (brick.origin[1] + int32_t(i / 4 % 4)) * 0.5f,
                    (brick.origin[2] + int32_t(i / 16)) * 0.5f};
      for (int axis = 0; axis < 3; ++axis) {
        CHECK(brick.voxels[i].depth[axis] ==
              doctest::Approx(bruteForceOpticalDepth(tris, {&material, 1}, lo, 0.5f, axis,
                                                     kVoxelRaysPerSide))
                  .epsilon(1e-5));
      }
      ++checked;
    }
  }
  CHECK(checked > 20);
}

TEST_CASE("an alpha-masked quad covers its alpha fraction") {
  CpuImage image{2, 1, {255, 255, 255, 255, 255, 255, 255, 0}};
  VoxelMaterial material{};
  material.baseColor = &image;
  material.alphaMask = true;
  material.alphaCutoff = 0.5f;
  std::vector<VoxelTriangle> tris;
  quad(tris, {0.0f, 0.0f, 0.0f}, 1.0f, 2);
  const VoxelGrid grid = voxelize(tris, {&material, 1}, 1.0f);
  const VoxelPayload *v = voxelAt(grid, 0, 0, 0);
  REQUIRE(v != nullptr);
  const float blocked = 1.0f - std::exp(-v->depth[2]);
  CHECK(blocked == doctest::Approx(0.5f).epsilon(0.07));
}

TEST_CASE("albedo and transmission are hit means") {
  VoxelMaterial red{}, blue{};
  red.baseColorFactor = {1.0f, 0.0f, 0.0f};
  red.diffuseTransmission = 0.5f;
  red.transmissionColor = {1.0f, 1.0f, 1.0f};
  blue.baseColorFactor = {0.0f, 0.0f, 1.0f};
  const VoxelMaterial materials[] = {red, blue};
  std::vector<VoxelTriangle> tris;
  quad(tris, {0.0f, 0.0f, 0.1f}, 1.0f, 2, 0);
  quad(tris, {0.0f, 0.0f, 0.6f}, 1.0f, 2, 1);
  const VoxelGrid grid = voxelize(tris, materials, 1.0f);
  const VoxelPayload *v = voxelAt(grid, 0, 0, 0);
  REQUIRE(v != nullptr);
  // Red reflects (1 - 0.5) of its albedo and transmits 0.5; blue transmits nothing.
  CHECK(v->albedo.x == doctest::Approx(0.25f).epsilon(0.02));
  CHECK(v->albedo.z == doctest::Approx(0.5f).epsilon(0.02));
  CHECK(v->transmission.x == doctest::Approx(0.25f).epsilon(0.02));
}

TEST_CASE("a merged voxel keeps its columns' mean transmittance") {
  const std::vector<VoxelTriangle> tris = leafSoup(400, 2.0f, 2);
  const VoxelMaterial material{};
  const VoxelGrid fine = voxelize(tris, {&material, 1}, 0.25f);
  const VoxelGrid coarse = mergeVoxelGrid(fine);
  CHECK(coarse.edge == 0.5f);
  size_t checked = 0;
  for (const VoxelBrick &brick : coarse.bricks) {
    for (uint32_t i = 0; i < 64; ++i) {
      if (!((brick.occupied >> i) & 1u)) {
        continue;
      }
      const int32_t c[3] = {brick.origin[0] + int32_t(i % 4), brick.origin[1] + int32_t(i / 4 % 4),
                            brick.origin[2] + int32_t(i / 16)};
      for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3, w = (axis + 2) % 3;
        float mean = 0.0f;
        for (int a = 0; a < 2; ++a) {
          for (int b = 0; b < 2; ++b) {
            float depth = 0.0f;
            for (int k = 0; k < 2; ++k) {
              int32_t f[3];
              f[axis] = 2 * c[axis] + k;
              f[u] = 2 * c[u] + a;
              f[w] = 2 * c[w] + b;
              const VoxelPayload *child = voxelAt(fine, f[0], f[1], f[2]);
              depth += child ? child->depth[axis] : 0.0f;
            }
            mean += 0.25f * std::exp(-depth);
          }
        }
        CHECK(brick.voxels[i].depth[axis] ==
              doctest::Approx(-std::log(std::max(mean, 1.0f / 32.0f))).epsilon(1e-4));
      }
      ++checked;
    }
  }
  CHECK(checked > 10);
}

TEST_CASE("the lattice is anchored at the origin") {
  std::vector<VoxelTriangle> tris;
  quad(tris, {0.30f, 0.05f, 0.05f}, 0.1f, 0);
  const VoxelMaterial material{};
  const VoxelGrid grid = voxelize(tris, {&material, 1}, 0.25f);
  REQUIRE(grid.bricks.size() == 1);
  CHECK(grid.bricks[0].origin[0] == 0);
  CHECK(voxelAt(grid, 1, 0, 0) != nullptr);
  std::vector<VoxelTriangle> negative;
  quad(negative, {-0.30f, 0.05f, 0.05f}, 0.1f, 0);
  const VoxelGrid g2 = voxelize(negative, {&material, 1}, 0.25f);
  REQUIRE(g2.bricks.size() == 1);
  CHECK(g2.bricks[0].origin[0] == -4);
  CHECK(voxelAt(g2, -2, 0, 0) != nullptr);
}

TEST_CASE("the normal moment of aligned leaves has alignment 1, of a sphere of leaves about 1/3") {
  std::vector<VoxelTriangle> aligned;
  quad(aligned, {0.0f, 0.0f, 0.3f}, 1.0f, 2);
  const VoxelMaterial material{};
  const VoxelGrid a = voxelize(aligned, {&material, 1}, 1.0f);
  const VoxelPayload *va = voxelAt(a, 0, 0, 0);
  REQUIRE(va != nullptr);
  const NormalLobe la = normalLobe(*va);
  CHECK(la.alignment == doctest::Approx(1.0f).epsilon(1e-3));
  CHECK(std::abs(la.axis.z) == doctest::Approx(1.0f).epsilon(1e-3));

  const VoxelGrid s = voxelize(leafSoup(3000, 1.0f, 3), {&material, 1}, 1.0f);
  REQUIRE(!s.bricks.empty());
  const VoxelPayload *vs = voxelAt(s, 0, 0, 0);
  REQUIRE(vs != nullptr);
  CHECK(normalLobe(*vs).alignment < 0.45f);
}

TEST_CASE("a packed voxel round-trips within its quantization") {
  VoxelPayload v;
  v.depth[0] = 0.5f, v.depth[1] = 2.0f, v.depth[2] = 3.4f;
  v.albedo = {0.04f, 0.09f, 0.02f};
  v.transmission = {0.10f, 0.20f, 0.03f};
  v.roughness = 0.6f;
  const Vec3 axis = normalize(Vec3{0.3f, 0.8f, -0.5f});
  const float a = 0.7f, rest = (1.0f - a) / 2.0f; // eigenvalues a, rest, rest
  v.moment[0] = rest + (a - rest) * axis.x * axis.x;
  v.moment[1] = rest + (a - rest) * axis.y * axis.y;
  v.moment[2] = rest + (a - rest) * axis.z * axis.z;
  v.moment[3] = (a - rest) * axis.x * axis.y;
  v.moment[4] = (a - rest) * axis.x * axis.z;
  v.moment[5] = (a - rest) * axis.y * axis.z;
  const VoxelPayload u = unpackVoxelPayload(packVoxelPayload(v));
  for (int i = 0; i < 3; ++i) {
    CHECK(std::abs(u.depth[i] - v.depth[i]) <= 1.0f / 32.0f);
  }
  // sqrt encoding: dark albedo within ~2% relative + 1/255^2 absolute.
  CHECK(u.albedo.y == doctest::Approx(0.09f).epsilon(0.03));
  CHECK(u.albedo.z == doctest::Approx(0.02f).epsilon(0.06));
  CHECK(u.transmission.x == doctest::Approx(0.10f).epsilon(0.03));
  CHECK(u.roughness == doctest::Approx(0.6f).epsilon(0.01));
  const NormalLobe lobe = normalLobe(u);
  CHECK(std::abs(dot(lobe.axis, axis)) > 0.999f);
  CHECK(lobe.alignment == doctest::Approx(0.7f).epsilon(0.01));
}

TEST_CASE("a grid's packed bricks unpack to its voxels") {
  std::vector<VoxelTriangle> a;
  quad(a, {0.0f, 0.0f, 0.2f}, 3.0f, 2);
  const VoxelMaterial material{};
  const VoxelGrid grid = voxelize(a, {&material, 1}, 0.5f);
  REQUIRE(grid.bricks.size() > 1);
  std::vector<VoxelBrickRef> bricks{VoxelBrickRef{}}; // appends after what's there
  std::vector<PackedVoxel> voxels(3);
  packVoxelBricks(grid, bricks, voxels);
  const VoxelGrid back =
      unpackVoxelBricks(std::span(bricks).subspan(1), voxels, grid.edge);
  CHECK(back.edge == grid.edge);
  REQUIRE(back.bricks.size() == grid.bricks.size());
  for (size_t b = 0; b < grid.bricks.size(); ++b) {
    CHECK(back.bricks[b].occupied == grid.bricks[b].occupied);
    CHECK(std::equal(std::begin(back.bricks[b].origin), std::end(back.bricks[b].origin),
                     std::begin(grid.bricks[b].origin)));
    uint32_t next = bricks[b + 1].firstVoxel;
    for (uint32_t i = 0; i < 64; ++i) {
      if ((grid.bricks[b].occupied >> i) & 1u) {
        const PackedVoxel p = packVoxelPayload(grid.bricks[b].voxels[i]);
        CHECK(std::memcmp(&p, &voxels[next], sizeof(p)) == 0);
        const VoxelPayload u = unpackVoxelPayload(voxels[next++]);
        CHECK(back.bricks[b].voxels[i].depth[2] == u.depth[2]);
        CHECK(back.bricks[b].voxels[i].albedo.y == u.albedo.y);
      }
    }
  }
}

TEST_CASE("combined grids add coincident voxels' optical depths, as independent media") {
  std::vector<VoxelTriangle> a, b;
  quad(a, {0.0f, 0.0f, 0.2f}, 1.0f, 2);
  quad(b, {0.0f, 0.0f, 0.7f}, 0.5f, 2);
  const VoxelMaterial material{};
  const VoxelGrid ga = voxelize(a, {&material, 1}, 1.0f);
  const VoxelGrid gb = voxelize(b, {&material, 1}, 1.0f);
  const VoxelGrid *grids[] = {&ga, &gb};
  const VoxelGrid both = combineVoxelGrids(grids);
  const VoxelPayload *va = voxelAt(ga, 0, 0, 0), *vb = voxelAt(gb, 0, 0, 0), *v = voxelAt(both, 0, 0, 0);
  REQUIRE(va);
  REQUIRE(vb);
  REQUIRE(v);
  CHECK(v->depth[2] == doctest::Approx(std::min(va->depth[2] + vb->depth[2], kOpaque)));
  CHECK(both.bricks.size() == 1);
}

TEST_CASE("a slanted plane blocks the analytic share of each voxel's rays") {
  // z = 0.75 x over x, y in [0, 4], edge 1: rays sit at (n + 0.5) / 4.
  std::vector<VoxelTriangle> tris = {tri({0, 0, 0}, {4, 0, 3}, {4, 4, 3}),
                                     tri({0, 0, 0}, {4, 4, 3}, {0, 4, 0})};
  const VoxelMaterial material{};
  const VoxelGrid grid = voxelize(tris, {&material, 1}, 1.0f);
  const auto depth = [](float blocked) { return -std::log(1.0f - blocked); };
  const VoxelPayload *low = voxelAt(grid, 1, 1, 0), *high = voxelAt(grid, 1, 1, 1);
  const VoxelPayload *next = voxelAt(grid, 2, 1, 1);
  REQUIRE(low);
  REQUIRE(high);
  REQUIRE(next);
  // Along z, column x in [1, 2): x = 1.125 hits at z = 0.84 (voxel 0), the
  // other three columns of rays at z = 1.03, 1.22, 1.41 (voxel 1).
  CHECK(low->depth[2] == doctest::Approx(depth(4.0f / 16.0f)));
  CHECK(high->depth[2] == doctest::Approx(depth(12.0f / 16.0f)));
  // Along x, x = z / 0.75: rows z = 0.125..0.875 hit at x = 0.17, 0.5, 0.83
  // (voxel 0) and 1.17 (voxel 1); rows z = 1.125..1.875 at x = 1.5, 1.83
  // (voxel 1) and 2.17, 2.5 (voxel 2).
  CHECK(low->depth[0] == doctest::Approx(depth(4.0f / 16.0f)));
  CHECK(high->depth[0] == doctest::Approx(depth(8.0f / 16.0f)));
  CHECK(next->depth[0] == doctest::Approx(depth(8.0f / 16.0f)));
  // The plane contains y: rays along y never hit it.
  CHECK(low->depth[1] == 0.0f);
  CHECK(high->depth[1] == 0.0f);
}

TEST_CASE("voxelize rejects a mesh beyond the voxel key's range") {
  const std::vector<VoxelTriangle> tris = {tri({0, 0, 0}, {1, 0, 0}, {0, 1, 0}),
                                           tri({0, 0, 2.0e6f}, {1, 0, 2.0e6f}, {0, 1, 2.0e6f})};
  const VoxelMaterial material{};
  CHECK_THROWS(voxelize(tris, {&material, 1}, 1.0f));
}
