#include <meridian/voxelize.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace meridian {

namespace {

float comp(Vec3 v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

float srgbToLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Bilinear, repeat-wrapped, in [0, 1] per channel.
std::array<float, 4> sample(const CpuImage &image, float u, float v) {
  const float x = u * image.width - 0.5f, y = v * image.height - 0.5f;
  const float fx = std::floor(x), fy = std::floor(y);
  const float tx = x - fx, ty = y - fy;
  const auto wrap = [](int64_t i, uint32_t n) {
    return static_cast<uint32_t>(((i % n) + n) % n);
  };
  std::array<float, 4> out{};
  for (int dy = 0; dy < 2; ++dy) {
    for (int dx = 0; dx < 2; ++dx) {
      const float w = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
      const uint32_t px = wrap(static_cast<int64_t>(fx) + dx, image.width);
      const uint32_t py = wrap(static_cast<int64_t>(fy) + dy, image.height);
      const uint8_t *texel = &image.rgba[(size_t{py} * image.width + px) * 4];
      for (int c = 0; c < 4; ++c) {
        out[c] += w * texel[c] * (1.0f / 255.0f);
      }
    }
  }
  return out;
}

struct Surface {
  bool opaque;
  Vec3 albedo, transmission;
  float roughness;
};

Surface surfaceAt(const VoxelMaterial &m, float u, float v) {
  Vec3 base = m.baseColorFactor;
  float alpha = 1.0f;
  if (m.baseColor) {
    const auto t = sample(*m.baseColor, u, v);
    base = {base.x * srgbToLinear(t[0]), base.y * srgbToLinear(t[1]), base.z * srgbToLinear(t[2])};
    alpha = t[3];
  }
  if (m.alphaMask && alpha < m.alphaCutoff) {
    return {false, {}, {}, 0.0f};
  }
  Vec3 transmitted = m.transmissionColor * m.diffuseTransmission;
  if (m.transmissionColorImage) {
    const auto t = sample(*m.transmissionColorImage, u, v);
    transmitted = {transmitted.x * srgbToLinear(t[0]), transmitted.y * srgbToLinear(t[1]),
                   transmitted.z * srgbToLinear(t[2])};
  }
  float roughness = m.roughnessFactor;
  if (m.metallicRoughness) {
    roughness *= sample(*m.metallicRoughness, u, v)[1];
  }
  return {true, base * (1.0f - m.diffuseTransmission), transmitted, roughness};
}

// A ray along `axis` at (pu, pw) in the plane of the other two axes (u =
// axis + 1, w = axis + 2) against a triangle: the hit's position along the
// axis and its barycentrics, or false. Doubles, so voxelize and the
// brute-force reference agree bit for bit.
struct PlaneHit {
  double z, w1, w2;
};
bool hitAlong(const VoxelTriangle &t, int axis, double pu, double pw, PlaneHit &hit) {
  const int u = (axis + 1) % 3, w = (axis + 2) % 3;
  const double ax = comp(t.p[0], u), ay = comp(t.p[0], w);
  const double e1x = comp(t.p[1], u) - ax, e1y = comp(t.p[1], w) - ay;
  const double e2x = comp(t.p[2], u) - ax, e2y = comp(t.p[2], w) - ay;
  const double det = e1x * e2y - e2x * e1y;
  if (det == 0.0) {
    return false;
  }
  const double du = pu - ax, dw = pw - ay;
  const double w1 = (du * e2y - e2x * dw) / det;
  const double w2 = (e1x * dw - du * e1y) / det;
  if (w1 < 0.0 || w2 < 0.0 || w1 + w2 > 1.0) {
    return false;
  }
  const double z0 = comp(t.p[0], axis);
  hit = {z0 + w1 * (comp(t.p[1], axis) - z0) + w2 * (comp(t.p[2], axis) - z0), w1, w2};
  return true;
}

void hitUv(const VoxelTriangle &t, const PlaneHit &h, float &u, float &v) {
  const float w0 = static_cast<float>(1.0 - h.w1 - h.w2);
  const float w1 = static_cast<float>(h.w1), w2 = static_cast<float>(h.w2);
  u = w0 * t.uv[0][0] + w1 * t.uv[1][0] + w2 * t.uv[2][0];
  v = w0 * t.uv[0][1] + w1 * t.uv[1][1] + w2 * t.uv[2][1];
}

struct Accum {
  uint16_t blocked[3] = {}; // per axis: the face rays that hit inside
  uint32_t hits = 0;
  double albedo[3] = {}, transmission[3] = {}, moment[6] = {}, roughness = 0.0;
};

constexpr int64_t kKeyBias = int64_t{1} << 20;
uint64_t voxelKey(int64_t x, int64_t y, int64_t z) {
  return (uint64_t(x + kKeyBias) << 42) | (uint64_t(y + kKeyBias) << 21) | uint64_t(z + kKeyBias);
}
void unpackKey(uint64_t key, int64_t c[3]) {
  c[0] = int64_t(key >> 42) - kKeyBias;
  c[1] = int64_t((key >> 21) & 0x1FFFFF) - kKeyBias;
  c[2] = int64_t(key & 0x1FFFFF) - kKeyBias;
}

float depthFromBlocked(float blocked) {
  return -std::log(std::max(1.0f - blocked, kVoxelMinTransmittance));
}

float coverageOf(const float depth[3]) {
  return 1.0f - (std::exp(-depth[0]) + std::exp(-depth[1]) + std::exp(-depth[2])) / 3.0f;
}

using VoxelMap = std::unordered_map<uint64_t, Accum>;

void voxelizeRange(std::span<const VoxelTriangle> triangles,
                   std::span<const VoxelMaterial> materials, float edge, VoxelMap &out) {
  const double step = double(edge) / kVoxelRaysPerSide;
  for (const VoxelTriangle &t : triangles) {
    const VoxelMaterial &m = materials[t.material];
    for (int axis = 0; axis < 3; ++axis) {
      const int u = (axis + 1) % 3, w = (axis + 2) % 3;
      double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
      for (int v = 0; v < 3; ++v) {
        lo[0] = std::min<double>(lo[0], comp(t.p[v], u));
        hi[0] = std::max<double>(hi[0], comp(t.p[v], u));
        lo[1] = std::min<double>(lo[1], comp(t.p[v], w));
        hi[1] = std::max<double>(hi[1], comp(t.p[v], w));
      }
      // Rays sit at (n + 0.5) step.
      const auto n0 = static_cast<int64_t>(std::ceil(lo[0] / step - 0.5));
      const auto n1 = static_cast<int64_t>(std::floor(hi[0] / step - 0.5));
      const auto m0 = static_cast<int64_t>(std::ceil(lo[1] / step - 0.5));
      const auto m1 = static_cast<int64_t>(std::floor(hi[1] / step - 0.5));
      for (int64_t a = n0; a <= n1; ++a) {
        for (int64_t b = m0; b <= m1; ++b) {
          PlaneHit hit;
          if (!hitAlong(t, axis, (a + 0.5) * step, (b + 0.5) * step, hit)) {
            continue;
          }
          float hu, hv;
          hitUv(t, hit, hu, hv);
          const Surface s = surfaceAt(m, hu, hv);
          if (!s.opaque) {
            continue;
          }
          int64_t c[3];
          c[axis] = static_cast<int64_t>(std::floor(hit.z / edge));
          c[u] = floorDiv(a, kVoxelRaysPerSide);
          c[w] = floorDiv(b, kVoxelRaysPerSide);
          Accum &acc = out[voxelKey(c[0], c[1], c[2])];
          const int sub = int(a - c[u] * kVoxelRaysPerSide) +
                          kVoxelRaysPerSide * int(b - c[w] * kVoxelRaysPerSide);
          acc.blocked[axis] |= uint16_t(1u << sub);
          const float w0 = float(1.0 - hit.w1 - hit.w2);
          Vec3 n = t.n[0] * w0 + t.n[1] * float(hit.w1) + t.n[2] * float(hit.w2);
          const float len = length(n);
          n = len > 0.0f ? n * (1.0f / len) : normalize(cross(t.p[1] - t.p[0], t.p[2] - t.p[0]));
          ++acc.hits;
          acc.albedo[0] += s.albedo.x, acc.albedo[1] += s.albedo.y, acc.albedo[2] += s.albedo.z;
          acc.transmission[0] += s.transmission.x, acc.transmission[1] += s.transmission.y,
              acc.transmission[2] += s.transmission.z;
          acc.moment[0] += n.x * n.x, acc.moment[1] += n.y * n.y, acc.moment[2] += n.z * n.z;
          acc.moment[3] += n.x * n.y, acc.moment[4] += n.x * n.z, acc.moment[5] += n.y * n.z;
          acc.roughness += s.roughness;
        }
      }
    }
  }
}

void merge(VoxelMap &into, VoxelMap &from) {
  for (auto &[key, a] : from) {
    Accum &b = into[key];
    for (int i = 0; i < 3; ++i) {
      b.blocked[i] |= a.blocked[i];
      b.albedo[i] += a.albedo[i];
      b.transmission[i] += a.transmission[i];
    }
    for (int i = 0; i < 6; ++i) {
      b.moment[i] += a.moment[i];
    }
    b.roughness += a.roughness;
    b.hits += a.hits;
  }
}

// Gathers voxels by brick, sorted by brick origin (z, y, x).
template <typename Voxels, typename Fill>
std::vector<VoxelBrick> toBricks(const Voxels &voxels, Fill fill) {
  std::map<std::array<int64_t, 3>, VoxelBrick> bricks;
  for (const auto &[key, value] : voxels) {
    int64_t c[3];
    unpackKey(key, c);
    const std::array<int64_t, 3> b = {floorDiv(c[2], 4), floorDiv(c[1], 4), floorDiv(c[0], 4)};
    VoxelBrick &brick = bricks[b];
    brick.origin[0] = int32_t(b[2] * 4), brick.origin[1] = int32_t(b[1] * 4),
    brick.origin[2] = int32_t(b[0] * 4);
    const uint32_t i = uint32_t(c[0] - brick.origin[0]) +
                       4 * (uint32_t(c[1] - brick.origin[1]) + 4 * uint32_t(c[2] - brick.origin[2]));
    brick.occupied |= uint64_t{1} << i;
    fill(brick.voxels[i], value);
  }
  std::vector<VoxelBrick> out;
  out.reserve(bricks.size());
  for (auto &[b, brick] : bricks) {
    out.push_back(brick);
  }
  return out;
}

} // namespace

NormalLobe normalLobe(const VoxelPayload &v) {
  const float m[3][3] = {{v.moment[0], v.moment[3], v.moment[4]},
                         {v.moment[3], v.moment[1], v.moment[5]},
                         {v.moment[4], v.moment[5], v.moment[2]}};
  const float trace = v.moment[0] + v.moment[1] + v.moment[2];
  if (trace <= 0.0f) {
    return {};
  }
  // Power iteration from the largest diagonal axis.
  Vec3 x = v.moment[0] >= v.moment[1] && v.moment[0] >= v.moment[2] ? Vec3{1, 0, 0}
           : v.moment[1] >= v.moment[2]                             ? Vec3{0, 1, 0}
                                                                     : Vec3{0, 0, 1};
  float lambda = 0.0f;
  for (int i = 0; i < 32; ++i) {
    const Vec3 y{m[0][0] * x.x + m[0][1] * x.y + m[0][2] * x.z,
                 m[1][0] * x.x + m[1][1] * x.y + m[1][2] * x.z,
                 m[2][0] * x.x + m[2][1] * x.y + m[2][2] * x.z};
    lambda = length(y);
    if (lambda <= 0.0f) {
      return {};
    }
    x = y * (1.0f / lambda);
  }
  return {x, std::clamp(lambda / trace, 1.0f / 3.0f, 1.0f)};
}

VoxelGrid voxelize(std::span<const VoxelTriangle> triangles,
                   std::span<const VoxelMaterial> materials, float edge) {
  // voxelKey packs each lattice coordinate in 21 bits.
  for (const VoxelTriangle &t : triangles) {
    for (const Vec3 &p : t.p) {
      for (int axis = 0; axis < 3; ++axis) {
        const double c = std::floor(double(comp(p, axis)) / edge);
        if (c < -double(kKeyBias) || c >= double(kKeyBias)) {
          throw std::runtime_error("voxelize: a vertex lies beyond 2^20 voxels of the origin");
        }
      }
    }
  }
  const size_t threads =
      std::clamp<size_t>(std::thread::hardware_concurrency(), 1, triangles.size() / 4096 + 1);
  std::vector<VoxelMap> maps(threads);
  std::vector<std::thread> workers;
  const size_t chunk = (triangles.size() + threads - 1) / threads;
  for (size_t i = 0; i < threads; ++i) {
    const size_t begin = std::min(triangles.size(), i * chunk);
    const size_t end = std::min(triangles.size(), begin + chunk);
    workers.emplace_back(
        [&, i, begin, end] { voxelizeRange(triangles.subspan(begin, end - begin), materials, edge, maps[i]); });
  }
  for (std::thread &t : workers) {
    t.join();
  }
  for (size_t i = 1; i < threads; ++i) {
    merge(maps[0], maps[i]);
  }

  constexpr float kRays = kVoxelRaysPerSide * kVoxelRaysPerSide;
  VoxelGrid grid;
  grid.edge = edge;
  grid.bricks = toBricks(maps[0], [](VoxelPayload &v, const Accum &a) {
    for (int i = 0; i < 3; ++i) {
      v.depth[i] = depthFromBlocked(std::popcount(a.blocked[i]) / kRays);
    }
    const double inv = 1.0 / a.hits;
    v.albedo = {float(a.albedo[0] * inv), float(a.albedo[1] * inv), float(a.albedo[2] * inv)};
    v.transmission = {float(a.transmission[0] * inv), float(a.transmission[1] * inv),
                      float(a.transmission[2] * inv)};
    for (int i = 0; i < 6; ++i) {
      v.moment[i] = float(a.moment[i] * inv);
    }
    v.roughness = float(a.roughness * inv);
    v.coverage = coverageOf(v.depth);
  });
  return grid;
}

VoxelGrid mergeVoxelGrid(const VoxelGrid &fine) {
  std::unordered_map<uint64_t, const VoxelPayload *> lookup;
  for (const VoxelBrick &brick : fine.bricks) {
    for (uint32_t i = 0; i < 64; ++i) {
      if ((brick.occupied >> i) & 1u) {
        lookup[voxelKey(brick.origin[0] + int64_t(i % 4), brick.origin[1] + int64_t(i / 4 % 4),
                        brick.origin[2] + int64_t(i / 16))] = &brick.voxels[i];
      }
    }
  }
  std::unordered_map<uint64_t, bool> parents;
  for (const auto &[key, v] : lookup) {
    int64_t c[3];
    unpackKey(key, c);
    parents[voxelKey(floorDiv(c[0], 2), floorDiv(c[1], 2), floorDiv(c[2], 2))] = true;
  }
  VoxelGrid coarse;
  coarse.edge = fine.edge * 2.0f;
  coarse.bricks = toBricks(parents, [](VoxelPayload &, bool) {});
  for (VoxelBrick &brick : coarse.bricks) {
    for (uint32_t i = 0; i < 64; ++i) {
      if (!((brick.occupied >> i) & 1u)) {
        continue;
      }
      const int64_t c[3] = {brick.origin[0] + int64_t(i % 4), brick.origin[1] + int64_t(i / 4 % 4),
                            brick.origin[2] + int64_t(i / 16)};
      VoxelPayload &out = brick.voxels[i];
      const VoxelPayload *children[8];
      for (int k = 0; k < 8; ++k) {
        const auto it = lookup.find(
            voxelKey(2 * c[0] + (k & 1), 2 * c[1] + (k >> 1 & 1), 2 * c[2] + (k >> 2)));
        children[k] = it == lookup.end() ? nullptr : it->second;
      }
      for (int axis = 0; axis < 3; ++axis) {
        float mean = 0.0f;
        for (int a = 0; a < 2; ++a) {
          for (int b = 0; b < 2; ++b) {
            float depth = 0.0f;
            for (int k = 0; k < 2; ++k) {
              int bit[3];
              bit[axis] = k;
              bit[(axis + 1) % 3] = a;
              bit[(axis + 2) % 3] = b;
              const VoxelPayload *child = children[bit[0] | bit[1] << 1 | bit[2] << 2];
              depth += child ? child->depth[axis] : 0.0f;
            }
            mean += 0.25f * std::exp(-depth);
          }
        }
        out.depth[axis] = -std::log(std::max(mean, kVoxelMinTransmittance));
      }
      float weight = 0.0f;
      Vec3 albedo, transmission;
      float moment[6] = {}, roughness = 0.0f;
      for (const VoxelPayload *child : children) {
        if (!child) {
          continue;
        }
        const float w = std::max(child->coverage, 1e-6f);
        weight += w;
        albedo = albedo + child->albedo * w;
        transmission = transmission + child->transmission * w;
        for (int k = 0; k < 6; ++k) {
          moment[k] += child->moment[k] * w;
        }
        roughness += child->roughness * w;
      }
      out.albedo = albedo * (1.0f / weight);
      out.transmission = transmission * (1.0f / weight);
      for (int k = 0; k < 6; ++k) {
        out.moment[k] = moment[k] / weight;
      }
      out.roughness = roughness / weight;
      out.coverage = coverageOf(out.depth);
    }
  }
  return coarse;
}

VoxelGrid combineVoxelGrids(std::span<const VoxelGrid *const> grids) {
  struct Sum {
    float depth[3] = {};
    float weight = 0.0f;
    Vec3 albedo, transmission;
    float moment[6] = {};
    float roughness = 0.0f;
  };
  std::unordered_map<uint64_t, Sum> sums;
  for (const VoxelGrid *grid : grids) {
    for (const VoxelBrick &brick : grid->bricks) {
      for (uint32_t i = 0; i < 64; ++i) {
        if (!((brick.occupied >> i) & 1u)) {
          continue;
        }
        const VoxelPayload &v = brick.voxels[i];
        Sum &s = sums[voxelKey(brick.origin[0] + int64_t(i % 4), brick.origin[1] + int64_t(i / 4 % 4),
                               brick.origin[2] + int64_t(i / 16))];
        const float w = std::max(v.coverage, 1e-6f);
        for (int a = 0; a < 3; ++a) {
          s.depth[a] += v.depth[a];
        }
        s.weight += w;
        s.albedo = s.albedo + v.albedo * w;
        s.transmission = s.transmission + v.transmission * w;
        for (int k = 0; k < 6; ++k) {
          s.moment[k] += v.moment[k] * w;
        }
        s.roughness += v.roughness * w;
      }
    }
  }
  VoxelGrid out;
  out.edge = grids.empty() ? 0.0f : grids[0]->edge;
  const float opaque = -std::log(kVoxelMinTransmittance);
  out.bricks = toBricks(sums, [&](VoxelPayload &v, const Sum &s) {
    for (int a = 0; a < 3; ++a) {
      v.depth[a] = std::min(s.depth[a], opaque);
    }
    const float inv = 1.0f / s.weight;
    v.albedo = s.albedo * inv;
    v.transmission = s.transmission * inv;
    for (int k = 0; k < 6; ++k) {
      v.moment[k] = s.moment[k] * inv;
    }
    v.roughness = s.roughness * inv;
    v.coverage = coverageOf(v.depth);
  });
  return out;
}

float bruteForceOpticalDepth(std::span<const VoxelTriangle> triangles,
                             std::span<const VoxelMaterial> materials, Vec3 voxelLo,
                             float edge, int axis, int raysPerSide) {
  const int u = (axis + 1) % 3, w = (axis + 2) % 3;
  const double step = double(edge) / raysPerSide;
  const auto cell = [&](float lo) { return static_cast<int64_t>(std::llround(lo / edge)); };
  const int64_t index = cell(comp(voxelLo, axis));
  int blocked = 0;
  for (int a = 0; a < raysPerSide; ++a) {
    for (int b = 0; b < raysPerSide; ++b) {
      const double pu = (cell(comp(voxelLo, u)) * raysPerSide + a + 0.5) * step;
      const double pw = (cell(comp(voxelLo, w)) * raysPerSide + b + 0.5) * step;
      for (const VoxelTriangle &t : triangles) {
        PlaneHit hit;
        if (!hitAlong(t, axis, pu, pw, hit) ||
            static_cast<int64_t>(std::floor(hit.z / edge)) != index) {
          continue;
        }
        float hu, hv;
        hitUv(t, hit, hu, hv);
        if (surfaceAt(materials[t.material], hu, hv).opaque) {
          ++blocked;
          break;
        }
      }
    }
  }
  return depthFromBlocked(float(blocked) / float(raysPerSide * raysPerSide));
}

namespace {

uint32_t unorm(float v, uint32_t bits) {
  const float max = float((1u << bits) - 1u);
  return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * max));
}

// Octahedral encoding of a unit vector to 2 x 16 bits.
uint32_t packOctahedral(Vec3 n) {
  const float s = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
  float x = n.x / s, y = n.y / s;
  if (n.z < 0.0f) {
    const float ox = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
    const float oy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
    x = ox, y = oy;
  }
  return unorm(x * 0.5f + 0.5f, 16) | unorm(y * 0.5f + 0.5f, 16) << 16;
}

Vec3 unpackOctahedral(uint32_t p) {
  float x = float(p & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
  float y = float(p >> 16) / 65535.0f * 2.0f - 1.0f;
  const float z = 1.0f - std::abs(x) - std::abs(y);
  if (z < 0.0f) {
    const float ox = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
    const float oy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
    x = ox, y = oy;
  }
  return normalize(Vec3{x, y, z});
}

uint32_t packSqrtRgb(Vec3 c) {
  return unorm(std::sqrt(std::max(c.x, 0.0f)), 8) | unorm(std::sqrt(std::max(c.y, 0.0f)), 8) << 8 |
         unorm(std::sqrt(std::max(c.z, 0.0f)), 8) << 16;
}

Vec3 unpackSqrtRgb(uint32_t p) {
  const auto channel = [&](int shift) {
    const float s = float((p >> shift) & 0xFF) / 255.0f;
    return s * s;
  };
  return {channel(0), channel(8), channel(16)};
}

} // namespace

PackedVoxel packVoxelPayload(const VoxelPayload &v) {
  const NormalLobe lobe = normalLobe(v);
  PackedVoxel p;
  for (int i = 0; i < 3; ++i) {
    p.depthSpread |= std::min<uint32_t>(255, uint32_t(std::lround(v.depth[i] * 16.0f))) << (8 * i);
  }
  p.depthSpread |= unorm((lobe.alignment - 1.0f / 3.0f) * 1.5f, 8) << 24;
  p.albedoRoughness = packSqrtRgb(v.albedo) | unorm(v.roughness, 8) << 24;
  p.transmission = packSqrtRgb(v.transmission);
  p.normal = packOctahedral(lobe.axis);
  return p;
}

VoxelPayload unpackVoxelPayload(const PackedVoxel &p) {
  VoxelPayload v;
  for (int i = 0; i < 3; ++i) {
    v.depth[i] = float((p.depthSpread >> (8 * i)) & 0xFF) / 16.0f;
  }
  v.albedo = unpackSqrtRgb(p.albedoRoughness);
  v.roughness = float(p.albedoRoughness >> 24) / 255.0f;
  v.transmission = unpackSqrtRgb(p.transmission);
  const float a = float(p.depthSpread >> 24) / 255.0f / 1.5f + 1.0f / 3.0f;
  const float rest = (1.0f - a) * 0.5f;
  const Vec3 n = unpackOctahedral(p.normal);
  v.moment[0] = rest + (a - rest) * n.x * n.x;
  v.moment[1] = rest + (a - rest) * n.y * n.y;
  v.moment[2] = rest + (a - rest) * n.z * n.z;
  v.moment[3] = (a - rest) * n.x * n.y;
  v.moment[4] = (a - rest) * n.x * n.z;
  v.moment[5] = (a - rest) * n.y * n.z;
  v.coverage = coverageOf(v.depth);
  return v;
}

void packVoxelBricks(const VoxelGrid &grid, std::vector<VoxelBrickRef> &bricks,
                     std::vector<PackedVoxel> &voxels) {
  for (const VoxelBrick &brick : grid.bricks) {
    VoxelBrickRef ref;
    std::copy(std::begin(brick.origin), std::end(brick.origin), ref.origin);
    ref.firstVoxel = uint32_t(voxels.size());
    ref.occupied = brick.occupied;
    bricks.push_back(ref);
    for (uint64_t bits = brick.occupied; bits != 0; bits &= bits - 1) {
      voxels.push_back(packVoxelPayload(brick.voxels[std::countr_zero(bits)]));
    }
  }
}

VoxelGrid unpackVoxelBricks(std::span<const VoxelBrickRef> bricks,
                            std::span<const PackedVoxel> voxels, float edge) {
  VoxelGrid grid;
  grid.edge = edge;
  for (const VoxelBrickRef &ref : bricks) {
    VoxelBrick &brick = grid.bricks.emplace_back();
    std::copy(std::begin(ref.origin), std::end(ref.origin), brick.origin);
    brick.occupied = ref.occupied;
    uint32_t next = ref.firstVoxel;
    for (uint64_t bits = ref.occupied; bits != 0; bits &= bits - 1) {
      brick.voxels[std::countr_zero(bits)] = unpackVoxelPayload(voxels[next++]);
    }
  }
  return grid;
}

} // namespace meridian
