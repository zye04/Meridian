// CPU side of the mid tier (protovoxel.h).
#include <meridian/protovoxel.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace meridian {
namespace {

struct Dense {
  uint32_t dims[3] = {};
  std::vector<float> extinction;
  std::vector<Vec3> normal;
  std::vector<uint8_t> albedo;
  std::vector<float> occlusion;
  size_t index(uint32_t x, uint32_t y, uint32_t z) const {
    return x + size_t{dims[0]} * (y + size_t{dims[1]} * z);
  }
};

// Directions toward the sky, weighted by cosine: the zenith and six at 45
// degrees.
struct SkyDirection {
  Vec3 direction;
  float weight;
};
std::array<SkyDirection, 7> skyDirections() {
  std::array<SkyDirection, 7> out;
  out[0] = {{0.0f, 1.0f, 0.0f}, 1.0f};
  const float c = std::sqrt(0.5f);
  for (uint32_t i = 0; i < 6; ++i) {
    const float a = 2.0f * 3.14159265f * static_cast<float>(i) / 6.0f;
    out[i + 1] = {{c * std::cos(a), c, c * std::sin(a)}, c};
  }
  return out;
}

void bakeOcclusion(Dense &fine, float voxelSize, float reach) {
  const auto directions = skyDirections();
  float total = 0.0f;
  for (const SkyDirection &d : directions) {
    total += d.weight;
  }
  const auto steps = static_cast<uint32_t>(std::max(reach / voxelSize, 1.0f));
  fine.occlusion.assign(fine.extinction.size(), 1.0f);
  const int32_t sx = static_cast<int32_t>(fine.dims[0]), sy = static_cast<int32_t>(fine.dims[1]),
                sz = static_cast<int32_t>(fine.dims[2]);
  for (int32_t z = 0; z < sz; ++z) {
    for (int32_t y = 0; y < sy; ++y) {
      for (int32_t x = 0; x < sx; ++x) {
        if (fine.extinction[fine.index(x, y, z)] <= 0.0f) {
          continue;
        }
        float visible = 0.0f;
        for (const SkyDirection &d : directions) {
          float depth = 0.0f;
          for (uint32_t s = 1; s <= steps && depth < 8.0f; ++s) {
            const float t = static_cast<float>(s) * voxelSize;
            const int32_t i = x + static_cast<int32_t>(std::floor(d.direction.x * t / voxelSize + 0.5f));
            const int32_t j = y + static_cast<int32_t>(std::floor(d.direction.y * t / voxelSize + 0.5f));
            const int32_t k = z + static_cast<int32_t>(std::floor(d.direction.z * t / voxelSize + 0.5f));
            if (i < 0 || j < 0 || k < 0 || i >= sx || j >= sy || k >= sz) {
              if (j >= sy) {
                break;
              }
              continue;
            }
            depth += fine.extinction[fine.index(i, j, k)] * voxelSize;
          }
          visible += d.weight * std::exp(-depth);
        }
        fine.occlusion[fine.index(x, y, z)] = visible / total;
      }
    }
  }
}

Dense downsample(const Dense &in) {
  Dense out;
  for (int a = 0; a < 3; ++a) {
    out.dims[a] = in.dims[a] / 2;
  }
  const size_t count = size_t{out.dims[0]} * out.dims[1] * out.dims[2];
  out.extinction.assign(count, 0.0f);
  out.normal.assign(count, Vec3{});
  out.albedo.assign(count, 0);
  out.occlusion.assign(count, 1.0f);
  for (uint32_t z = 0; z < out.dims[2]; ++z) {
    for (uint32_t y = 0; y < out.dims[1]; ++y) {
      for (uint32_t x = 0; x < out.dims[0]; ++x) {
        double sum = 0.0, occlusion = 0.0;
        Vec3 normal;
        std::array<std::pair<uint8_t, float>, 8> albedo{};
        uint32_t albedoCount = 0;
        for (uint32_t c = 0; c < 8; ++c) {
          const size_t i = in.index(2 * x + (c & 1), 2 * y + ((c >> 1) & 1), 2 * z + (c >> 2));
          const float e = in.extinction[i];
          if (e <= 0.0f) {
            continue;
          }
          sum += e;
          occlusion += e * in.occlusion[i];
          normal = normal + in.normal[i] * e;
          uint32_t slot = 0;
          while (slot < albedoCount && albedo[slot].first != in.albedo[i]) {
            ++slot;
          }
          if (slot == albedoCount) {
            albedo[albedoCount++] = {in.albedo[i], 0.0f};
          }
          albedo[slot].second += e;
        }
        if (sum <= 0.0) {
          continue;
        }
        const size_t o = out.index(x, y, z);
        out.extinction[o] = static_cast<float>(sum / 8.0);
        out.occlusion[o] = static_cast<float>(occlusion / sum);
        out.normal[o] = length(normal) > 0.0f ? normalize(normal) : Vec3{0.0f, 1.0f, 0.0f};
        uint32_t best = 0;
        for (uint32_t s = 1; s < albedoCount; ++s) {
          best = albedo[s].second > albedo[best].second ? s : best;
        }
        out.albedo[o] = albedo[best].first;
      }
    }
  }
  return out;
}

// Per voxel and axis direction (+x, -x, +y, -y, +z, -z), the optical depth
// from its center out of the box: half its own voxel plus every voxel beyond.
std::vector<std::array<float, 6>> axisDepths(const Dense &dense, float voxelSize) {
  std::vector<std::array<float, 6>> out(dense.extinction.size());
  for (int axis = 0; axis < 3; ++axis) {
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    for (uint32_t u = 0; u < dense.dims[a1]; ++u) {
      for (uint32_t w = 0; w < dense.dims[a2]; ++w) {
        auto at = [&](uint32_t t) {
          uint32_t c[3];
          c[axis] = t;
          c[a1] = u;
          c[a2] = w;
          return dense.index(c[0], c[1], c[2]);
        };
        const uint32_t n = dense.dims[axis];
        float beyond = 0.0f;
        for (uint32_t t = n; t-- > 0;) {
          const float own = dense.extinction[at(t)] * voxelSize;
          out[at(t)][2 * axis] = beyond + 0.5f * own;
          beyond += own;
        }
        beyond = 0.0f;
        for (uint32_t t = 0; t < n; ++t) {
          const float own = dense.extinction[at(t)] * voxelSize;
          out[at(t)][2 * axis + 1] = beyond + 0.5f * own;
          beyond += own;
        }
      }
    }
  }
  return out;
}

void packLevel(const Dense &dense, float voxelSize, PrototypeVoxels &out) {
  const auto depths = axisDepths(dense, voxelSize);
  ProtoVoxelLevel level;
  level.voxelSize = voxelSize;
  for (int a = 0; a < 3; ++a) {
    level.dims[a] = dense.dims[a];
    level.bricks[a] = (dense.dims[a] + kBrickEdge - 1) / kBrickEdge;
  }
  level.gridOffset = static_cast<uint32_t>(out.brickGrid.size());
  level.brickOffset = out.brickCount;
  out.brickGrid.resize(out.brickGrid.size() + size_t{level.bricks[0]} * level.bricks[1] * level.bricks[2],
                       kEmptyBrick);
  for (uint32_t bz = 0; bz < level.bricks[2]; ++bz) {
    for (uint32_t by = 0; by < level.bricks[1]; ++by) {
      for (uint32_t bx = 0; bx < level.bricks[0]; ++bx) {
        std::array<uint32_t, kBrickVoxels * 2> brick{};
        bool occupied = false;
        for (uint32_t i = 0; i < kBrickVoxels; ++i) {
          const uint32_t x = bx * 4 + (i & 3), y = by * 4 + ((i >> 2) & 3), z = bz * 4 + (i >> 4);
          if (x >= dense.dims[0] || y >= dense.dims[1] || z >= dense.dims[2]) {
            continue;
          }
          const size_t d = dense.index(x, y, z);
          if (dense.extinction[d] <= 0.0f) {
            continue;
          }
          const uint32_t packed = packVoxel(dense.extinction[d], dense.normal[d], dense.albedo[d]);
          if ((packed & 0xFF) == 0) {
            continue;
          }
          occupied = true;
          brick[2 * i] = packed;
          brick[2 * i + 1] =
              static_cast<uint32_t>(std::clamp(dense.occlusion[d], 0.0f, 1.0f) * 255.0f + 0.5f);
          for (uint32_t k = 0; k < 6; ++k) {
            const auto q = static_cast<uint32_t>(
                std::min(depths[d][k] / kProtoAxisDepthStep + 0.5f, 15.0f));
            brick[2 * i + 1] |= q << (8 + 4 * k);
          }
        }
        if (occupied) {
          out.brickGrid[level.gridOffset + bx + level.bricks[0] * (by + level.bricks[1] * bz)] =
              out.brickCount++ - level.brickOffset;
          out.voxels.insert(out.voxels.end(), brick.begin(), brick.end());
        }
      }
    }
  }
  out.levels.push_back(level);
}

} // namespace

uint32_t protoLevelCount(float finestVoxel) {
  uint32_t levels = 1;
  while (levels < kProtoMaxLevels &&
         finestVoxel * static_cast<float>(1u << levels) <= kProtoCoarsestVoxel * 1.125f) {
    ++levels;
  }
  return levels;
}

PrototypeVoxels buildPrototypeVoxels(const ProtoBakeGrid &grid) {
  PrototypeVoxels out;
  out.boundsLo = grid.origin;
  out.boundsHi = grid.origin + Vec3{static_cast<float>(grid.size[0]), static_cast<float>(grid.size[1]),
                                    static_cast<float>(grid.size[2])} *
                                   grid.voxelSize;
  Dense dense;
  std::copy(std::begin(grid.size), std::end(grid.size), dense.dims);
  dense.extinction = grid.extinction;
  dense.normal = grid.normal;
  dense.albedo = grid.albedo;
  const float height = static_cast<float>(grid.size[1]) * grid.voxelSize;
  bakeOcclusion(dense, grid.voxelSize, 0.35f * height);
  const uint32_t levels = protoLevelCount(grid.voxelSize);
  for (uint32_t level = 0; level < levels; ++level) {
    packLevel(dense, grid.voxelSize * static_cast<float>(1u << level), out);
    if (level + 1 < levels) {
      dense = downsample(dense);
    }
  }
  return out;
}

uint32_t protoVoxelAt(const PrototypeVoxels &voxels, uint32_t level, int32_t x, int32_t y,
                      int32_t z, uint32_t *occlusion) {
  const ProtoVoxelLevel &l = voxels.levels[level];
  if (occlusion) {
    *occlusion = 255;
  }
  if (x < 0 || y < 0 || z < 0 || x >= static_cast<int32_t>(l.bricks[0] * 4) ||
      y >= static_cast<int32_t>(l.bricks[1] * 4) || z >= static_cast<int32_t>(l.bricks[2] * 4)) {
    return 0;
  }
  const uint32_t brick = voxels.brickGrid[l.gridOffset + (x >> 2) +
                                          l.bricks[0] * ((y >> 2) + l.bricks[1] * (z >> 2))];
  if (brick == kEmptyBrick) {
    return 0;
  }
  const size_t i = 2 * (size_t{l.brickOffset + brick} * kBrickVoxels + (x & 3) + 4 * ((y & 3) + 4 * (z & 3)));
  if (occlusion) {
    *occlusion = voxels.voxels[i + 1];
  }
  return voxels.voxels[i];
}

namespace {

// Clips the ray to the prototype's box; false if it misses.
bool clipToBox(const PrototypeVoxels &voxels, Vec3 origin, Vec3 direction, float tMax, float &t0,
               float &t1) {
  t0 = 0.0f;
  t1 = tMax;
  const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {direction.x, direction.y, direction.z};
  const float lo[3] = {voxels.boundsLo.x, voxels.boundsLo.y, voxels.boundsLo.z};
  const float hi[3] = {voxels.boundsHi.x, voxels.boundsHi.y, voxels.boundsHi.z};
  for (int a = 0; a < 3; ++a) {
    if (std::abs(d[a]) < 1e-12f) {
      if (o[a] < lo[a] || o[a] > hi[a]) {
        return false;
      }
      continue;
    }
    float a0 = (lo[a] - o[a]) / d[a], a1 = (hi[a] - o[a]) / d[a];
    if (a0 > a1) {
      std::swap(a0, a1);
    }
    t0 = std::max(t0, a0);
    t1 = std::min(t1, a1);
  }
  return t0 < t1;
}

} // namespace

ProtoHit marchPrototypeVoxelsBrute(const PrototypeVoxels &voxels, uint32_t level, Vec3 origin,
                                   Vec3 direction, float tMax, float limit) {
  ProtoHit result;
  float t0, t1;
  if (!clipToBox(voxels, origin, direction, tMax, t0, t1)) {
    return result;
  }
  const float v = voxels.levels[level].voxelSize;
  const float h = v / 64.0f;
  float opticalDepth = 0.0f;
  for (float t = t0 + 0.5f * h; t < t1; t += h) {
    const Vec3 p = (origin + direction * t - voxels.boundsLo) * (1.0f / v);
    uint32_t occlusion = 0;
    const uint32_t packed = protoVoxelAt(voxels, level, static_cast<int32_t>(std::floor(p.x)),
                                         static_cast<int32_t>(std::floor(p.y)),
                                         static_cast<int32_t>(std::floor(p.z)), &occlusion);
    const float segment = unpackVoxel(packed).extinction * h;
    if (packed != 0 && opticalDepth + segment >= limit) {
      result.hit = true;
      result.t = t;
      result.voxel = packed;
      result.occlusion = occlusion;
      result.opticalDepth = limit;
      return result;
    }
    opticalDepth += segment;
  }
  result.opticalDepth = opticalDepth;
  return result;
}

float voxelSwitchDistance(float finestVoxel, float scale, float pixelsPerMeter) {
  return finestVoxel * scale * pixelsPerMeter / kVoxelSwitchPixels;
}

float voxelBlend(float distance, float switchDistance) {
  return std::clamp((distance - switchDistance) / (kVoxelSwitchBand * switchDistance), 0.0f, 1.0f);
}

float protoVoxelDepth(uint32_t occlusionWord, Vec3 d) {
  const float c[3] = {d.x, d.y, d.z};
  float depth = 0.0f;
  for (int a = 0; a < 3; ++a) {
    const uint32_t shift = 8 + 4 * (2 * a + (c[a] < 0.0f ? 1 : 0));
    depth += c[a] * c[a] * static_cast<float>((occlusionWord >> shift) & 15u) * kProtoAxisDepthStep;
  }
  return depth;
}

uint32_t voxelMip(float distance, float finestVoxel, float scale, float pixelsPerMeter,
                  uint32_t levelCount) {
  const float ratio = kVoxelMipPixels * distance / (pixelsPerMeter * finestVoxel * scale);
  const float level = std::floor(std::log2(std::max(ratio, 1.0f)));
  return std::min(static_cast<uint32_t>(level), levelCount - 1);
}

} // namespace meridian
