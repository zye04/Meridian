// CPU side of the far field (farfield.h). The splat's footprint, deposit
// and pack mirror farfield_splat.comp operation for operation, so Debug
// builds can compare the GPU result with this replica.
#include <meridian/farfield.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace meridian {
namespace {

uint32_t extinctionCode(float extinction) {
  if (!(extinction > 0.0f)) {
    return 0;
  }
  const float code = std::floor(18.0f * (std::log2(extinction) + 10.0f) + 0.5f) + 1.0f;
  return code < 1.0f ? 0u : static_cast<uint32_t>(std::min(code, 255.0f));
}

float signNotZero(float value) { return value >= 0.0f ? 1.0f : -1.0f; }

uint32_t octahedralByte(float value) {
  return static_cast<uint32_t>(std::floor((value * 0.5f + 0.5f) * 255.0f + 0.5f));
}

Vec3 rotateByQuaternion(const std::array<float, 4> &q, Vec3 v) {
  const Vec3 axis{q[0], q[1], q[2]};
  return v + cross(axis, cross(axis, v) + v * q[3]) * 2.0f;
}

int32_t floorDiv4(int32_t value) { return value >= 0 ? value / 4 : -((3 - value) / 4); }

int32_t fixedPoint(float value) { return static_cast<int32_t>(std::floor(value + 0.5f)); }

// The voxel at global voxel (i, j) and column-local layer k; 0 outside.
uint32_t voxelAt(const FarFieldCpu &field, uint32_t level, const BrickColumn &column, int32_t i,
                 int32_t k, int32_t j) {
  if (k < 0 || k >= static_cast<int32_t>(columnBrickCount(column) * kBrickEdge)) {
    return 0;
  }
  const size_t brick = column.firstBrick + static_cast<uint32_t>(k) / kBrickEdge;
  const uint32_t local = (i & 3) + 4 * ((k & 3) + 4 * (j & 3));
  return field.voxels[level][brick * kBrickVoxels + local];
}

} // namespace

uint32_t packVoxel(float extinction, Vec3 normal, uint8_t albedoIndex) {
  const float l1 = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
  float px = normal.x / l1, py = normal.y / l1;
  if (normal.z < 0.0f) {
    const float qx = (1.0f - std::abs(py)) * signNotZero(px);
    py = (1.0f - std::abs(px)) * signNotZero(py);
    px = qx;
  }
  return extinctionCode(extinction) | octahedralByte(px) << 8 | octahedralByte(py) << 16 |
         uint32_t{albedoIndex} << 24;
}

Voxel unpackVoxel(uint32_t voxel) {
  const uint32_t code = voxel & 0xFF;
  Voxel out;
  out.extinction = code == 0 ? 0.0f : std::exp2(static_cast<float>(code - 1) / 18.0f - 10.0f);
  Vec3 n{static_cast<float>((voxel >> 8) & 0xFF) / 255.0f * 2.0f - 1.0f,
         static_cast<float>((voxel >> 16) & 0xFF) / 255.0f * 2.0f - 1.0f, 0.0f};
  n.z = 1.0f - std::abs(n.x) - std::abs(n.y);
  const float t = std::max(-n.z, 0.0f);
  n.x += n.x >= 0.0f ? -t : t;
  n.y += n.y >= 0.0f ? -t : t;
  out.normal = normalize(n);
  out.albedoIndex = static_cast<uint8_t>(voxel >> 24);
  return out;
}

float farExtinctionFromPasses(uint32_t passes, uint32_t rays) {
  if (passes >= rays) {
    return 0.0f;
  }
  const double pass = std::max(static_cast<double>(passes), 0.5) / rays;
  return static_cast<float>(-std::log(pass) / kFarTemplateVoxel);
}

FarTemplates filterFarTemplate(const FarBakeGrid &grid) {
  FarTemplates templates;
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    const auto f = static_cast<uint32_t>(kFarVoxelSize[level] / kFarTemplateVoxel);
    FarTemplate &out = templates[level];
    out.centerMin = {1e30f, 1e30f, 1e30f};
    out.centerMax = {-1e30f, -1e30f, -1e30f};
    const uint32_t size[3] = {(grid.size[0] + f - 1) / f, (grid.size[1] + f - 1) / f,
                              (grid.size[2] + f - 1) / f};
    for (uint32_t z = 0; z < size[2]; ++z) {
      for (uint32_t y = 0; y < size[1]; ++y) {
        for (uint32_t x = 0; x < size[0]; ++x) {
          double sum = 0.0;
          Vec3 normal;
          std::array<float, 256> albedo{};
          for (uint32_t cz = z * f; cz < std::min((z + 1) * f, grid.size[2]); ++cz) {
            for (uint32_t cy = y * f; cy < std::min((y + 1) * f, grid.size[1]); ++cy) {
              for (uint32_t cx = x * f; cx < std::min((x + 1) * f, grid.size[0]); ++cx) {
                const size_t i = cx + grid.size[0] * (cy + size_t{grid.size[1]} * cz);
                const float e = grid.extinction[i];
                sum += e;
                normal = normal + grid.normal[i] * e;
                albedo[grid.albedo[i]] += e;
              }
            }
          }
          if (sum <= 0.0) {
            continue;
          }
          FarTemplateVoxel voxel;
          voxel.center = grid.origin +
                         Vec3{(x + 0.5f) * f, (y + 0.5f) * f, (z + 0.5f) * f} * kFarTemplateVoxel;
          voxel.extinction = static_cast<float>(sum / std::pow(static_cast<double>(f), 3.0));
          voxel.normal = length(normal) > 0.0f ? normalize(normal) : Vec3{0.0f, 1.0f, 0.0f};
          voxel.albedo = static_cast<uint32_t>(std::max_element(albedo.begin(), albedo.end()) -
                                               albedo.begin());
          out.voxels.push_back(voxel);
          const Vec3 c = voxel.center;
          out.centerMin = {std::min(out.centerMin.x, c.x), std::min(out.centerMin.y, c.y),
                           std::min(out.centerMin.z, c.z)};
          out.centerMax = {std::max(out.centerMax.x, c.x), std::max(out.centerMax.y, c.y),
                           std::max(out.centerMax.z, c.z)};
        }
      }
    }
  }
  return templates;
}

FarFootprint farFootprint(const FarFieldGrid &grid, uint32_t level, const FarTemplate &tmpl,
                          const DecodedInstance &instance) {
  // A deposit's x/z may round past the corners' hull by a few ulps. Past
  // the height range it can't: the brick count leaves at least one unit
  // above, and a deposit just below the base has a weight that rounds to 0.
  constexpr float kPad = 0.01f;
  const float v = kFarVoxelSize[level];
  // Exact reciprocals (powers of two), so the GPU's operations match.
  const float inverseV = 1.0f / v, inverseUnit = 1.0f / kFarHeightUnit;
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (uint32_t corner = 0; corner < 8; ++corner) {
    const Vec3 c{corner & 1 ? tmpl.centerMax.x : tmpl.centerMin.x,
                 corner & 2 ? tmpl.centerMax.y : tmpl.centerMin.y,
                 corner & 4 ? tmpl.centerMax.z : tmpl.centerMin.z};
    const Vec3 p = rotateByQuaternion(instance.rotation, c * instance.scale) + instance.position;
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  FarFootprint out;
  out.x0 =
      floorDiv4(static_cast<int32_t>(std::floor((lo.x - kPad - grid.originX) * inverseV - 0.5f)));
  out.z0 =
      floorDiv4(static_cast<int32_t>(std::floor((lo.z - kPad - grid.originZ) * inverseV - 0.5f)));
  out.x1 = floorDiv4(
      static_cast<int32_t>(std::floor((hi.x + kPad - grid.originX) * inverseV - 0.5f)) + 1);
  out.z1 = floorDiv4(
      static_cast<int32_t>(std::floor((hi.z + kPad - grid.originZ) * inverseV - 0.5f)) + 1);
  const float bottom = std::floor((lo.y - 0.5f * v - grid.heightBase) * inverseUnit);
  const float top = std::ceil((hi.y + 0.5f * v - grid.heightBase) * inverseUnit);
  out.lo = static_cast<uint32_t>(std::max(bottom, 0.0f));
  out.hi = static_cast<uint32_t>(std::max(top, 0.0f));
  return out;
}

std::vector<BrickColumn> farBrickColumns(std::span<const uint32_t> loHi, uint32_t level,
                                         uint32_t &brickTotal) {
  const auto unitsPerBrick =
      static_cast<uint32_t>(kBrickEdge * kFarVoxelSize[level] / kFarHeightUnit);
  std::vector<BrickColumn> columns(loHi.size() / 2);
  for (size_t c = 0; c < columns.size(); ++c) {
    const uint32_t lo = loHi[2 * c], hi = loHi[2 * c + 1];
    if (lo == UINT32_MAX) {
      columns[c] = makeBrickColumn(0, 0, 0);
      continue;
    }
    const uint32_t count = (hi - lo) / unitsPerBrick + 1;
    if (lo > 0xFFFF || count > 0xFFFF ||
        uint64_t{brickTotal} + count >
            std::numeric_limits<uint32_t>::max() / (kBrickVoxels * kFarChannels)) {
      throw std::runtime_error("far field column out of range");
    }
    columns[c] = makeBrickColumn(brickTotal, lo, count);
    brickTotal += count;
  }
  return columns;
}

float farFixedScale(float maxExtinction, float scaleMin, float scaleMax,
                    uint32_t instancesPerVoxel) {
  // Template lattice points (spacing scaleMin voxels) inside one voxel's
  // trilinear support (a 2-voxel cube, in a ball of diameter 2 sqrt 3).
  const double lattice = std::pow(2.0 * std::sqrt(3.0) / scaleMin + 1.0, 3.0);
  const double worst = static_cast<double>(instancesPerVoxel) * lattice * maxExtinction * scaleMax *
                       scaleMax * scaleMax;
  return worst > 0.0 ? static_cast<float>(2147483647.0 / worst * 0.999) : 1.0f;
}

FarFieldCpu splatFarField(const FarFieldGrid &grid, std::span<const FarTemplates> templates,
                          std::span<const Vec3> palette, std::span<const DecodedInstance> instances,
                          const std::array<float, kFarLevels> &fixedScale,
                          std::array<double, kFarLevels> *extinctionSums,
                          std::array<int64_t, kFarLevels> *largestSums) {
  FarFieldCpu field;
  field.origin = {grid.originX, grid.heightBase, grid.originZ};
  field.cellsX = grid.cellsX;
  field.cellsZ = grid.cellsZ;
  field.heightBase = grid.heightBase;
  const size_t cellCount = size_t{grid.cellsX} * grid.cellsZ;
  field.cellBottom.assign(cellCount, std::numeric_limits<float>::max());
  field.cellTop.assign(cellCount, -std::numeric_limits<float>::max());

  for (uint32_t level = 0; level < kFarLevels; ++level) {
    const uint32_t n = farColumnsPerCell(level);
    const float v = kFarVoxelSize[level], inverseV = 1.0f / v;
    const float scale = fixedScale[level];
    const uint32_t columnsX = grid.cellsX * n, columnsZ = grid.cellsZ * n;
    const auto levelTemplate = [&](const DecodedInstance &instance) -> const FarTemplate * {
      if (instance.prototype >= templates.size() ||
          templates[instance.prototype][level].voxels.empty()) {
        return nullptr;
      }
      return &templates[instance.prototype][level];
    };

    // Occupancy.
    std::vector<uint32_t> loHi(size_t{2} * columnsX * columnsZ);
    for (size_t c = 0; c < loHi.size(); c += 2) {
      loHi[c] = UINT32_MAX;
      loHi[c + 1] = 0;
    }
    for (const DecodedInstance &instance : instances) {
      const FarTemplate *tmpl = levelTemplate(instance);
      if (!tmpl) {
        continue;
      }
      const FarFootprint fp = farFootprint(grid, level, *tmpl, instance);
      for (int32_t z = std::max(fp.z0, 0); z <= std::min(fp.z1, int32_t(columnsZ) - 1); ++z) {
        for (int32_t x = std::max(fp.x0, 0); x <= std::min(fp.x1, int32_t(columnsX) - 1); ++x) {
          const size_t c = farColumnIndex(grid.cellsX, level, x, z);
          loHi[2 * c] = std::min(loHi[2 * c], fp.lo);
          loHi[2 * c + 1] = std::max(loHi[2 * c + 1], fp.hi);
        }
      }
    }

    // Allocation.
    uint32_t bricks = 0;
    field.columns[level] = farBrickColumns(loHi, level, bricks);
    for (size_t c = 0; c < field.columns[level].size(); ++c) {
      if (loHi[2 * c] != UINT32_MAX) {
        const size_t cell = c / (n * n);
        field.cellBottom[cell] =
            std::min(field.cellBottom[cell], grid.heightBase + loHi[2 * c] * kFarHeightUnit);
        field.cellTop[cell] =
            std::max(field.cellTop[cell], grid.heightBase + loHi[2 * c + 1] * kFarHeightUnit);
      }
    }

    // Accumulation: each template voxel's extinction (times scale^3, the
    // density a scaled copy keeps) goes trilinearly to the 8 nearest world
    // voxel centers, vertically on its column's own lattice.
    // 64-bit here, so largestSums sees what the GPU's u32 sums would wrap;
    // packing reads them as the GPU does.
    std::vector<int64_t> acc(size_t{bricks} * kBrickVoxels * kFarChannels, 0);
    for (const DecodedInstance &instance : instances) {
      const FarTemplate *tmpl = levelTemplate(instance);
      if (!tmpl) {
        continue;
      }
      const float s = instance.scale;
      for (const FarTemplateVoxel &tv : tmpl->voxels) {
        const Vec3 p = rotateByQuaternion(instance.rotation, tv.center * s) + instance.position;
        const float value = tv.extinction * (s * s * s);
        const Vec3 normal = rotateByQuaternion(instance.rotation, tv.normal);
        const Vec3 albedo = palette[tv.albedo];
        const float fx = (p.x - grid.originX) * inverseV - 0.5f;
        const float fz = (p.z - grid.originZ) * inverseV - 0.5f;
        const float ix = std::floor(fx), iz = std::floor(fz);
        const float wx = fx - ix, wz = fz - iz;
        for (int32_t dz = 0; dz < 2; ++dz) {
          for (int32_t dx = 0; dx < 2; ++dx) {
            const float wh = (dx ? wx : 1.0f - wx) * (dz ? wz : 1.0f - wz);
            const int32_t i = static_cast<int32_t>(ix) + dx, j = static_cast<int32_t>(iz) + dz;
            if (wh <= 0.0f || i < 0 || j < 0 || i >= int32_t(columnsX * kBrickEdge) ||
                j >= int32_t(columnsZ * kBrickEdge)) {
              continue;
            }
            const BrickColumn &column =
                field.columns[level][farColumnIndex(grid.cellsX, level, i / 4, j / 4)];
            const float base = grid.heightBase + columnBaseHeight(column) * kFarHeightUnit;
            const float fy = (p.y - base) * inverseV - 0.5f;
            const float ky = std::floor(fy);
            const float wy = fy - ky;
            for (int32_t dy = 0; dy < 2; ++dy) {
              const float m = value * (wh * (dy ? wy : 1.0f - wy));
              const int32_t q = fixedPoint(m * scale);
              const int32_t k = static_cast<int32_t>(ky) + dy;
              if (q <= 0 || k < 0 || k >= int32_t(columnBrickCount(column) * kBrickEdge)) {
                continue;
              }
              const size_t brick = column.firstBrick + k / 4;
              const uint32_t local = (i & 3) + 4 * ((k & 3) + 4 * (j & 3));
              int64_t *a = &acc[(brick * kBrickVoxels + local) * kFarChannels];
              a[0] += q;
              a[1] += fixedPoint(m * normal.x * scale);
              a[2] += fixedPoint(m * normal.y * scale);
              a[3] += fixedPoint(m * normal.z * scale);
              a[4] += fixedPoint(m * albedo.x * scale);
              a[5] += fixedPoint(m * albedo.y * scale);
              a[6] += fixedPoint(m * albedo.z * scale);
            }
          }
        }
      }
    }

    // Pack.
    field.voxels[level].assign(size_t{bricks} * kBrickVoxels, 0);
    if (largestSums) {
      int64_t largest = 0;
      for (int64_t value : acc) {
        largest = std::max(largest, value < 0 ? -value : value);
      }
      (*largestSums)[level] = largest;
    }
    double sum = 0.0;
    for (size_t voxel = 0; voxel < field.voxels[level].size(); ++voxel) {
      uint32_t a[kFarChannels];
      for (uint32_t c = 0; c < kFarChannels; ++c) {
        a[c] = static_cast<uint32_t>(acc[voxel * kFarChannels + c]);
      }
      if (a[0] == 0) {
        continue;
      }
      sum += static_cast<double>(a[0]) / scale;
      const float e = static_cast<float>(a[0]);
      Vec3 normal{static_cast<float>(static_cast<int32_t>(a[1])),
                  static_cast<float>(static_cast<int32_t>(a[2])),
                  static_cast<float>(static_cast<int32_t>(a[3]))};
      normal = length(normal) > 0.0f ? normalize(normal) : Vec3{0.0f, 1.0f, 0.0f};
      const Vec3 albedo{static_cast<float>(a[4]) / e, static_cast<float>(a[5]) / e,
                        static_cast<float>(a[6]) / e};
      uint32_t best = 0;
      float bestDistance = 1e30f;
      for (uint32_t entry = 0; entry < palette.size(); ++entry) {
        const Vec3 d = palette[entry] - albedo;
        if (dot(d, d) < bestDistance) {
          bestDistance = dot(d, d);
          best = entry;
        }
      }
      field.voxels[level][voxel] = packVoxel(e / scale, normal, static_cast<uint8_t>(best));
    }
    if (extinctionSums) {
      (*extinctionSums)[level] = sum * v * v * v;
    }
  }
  return field;
}

float farFieldExtinction(const FarFieldCpu &field, uint32_t level, Vec3 position) {
  const float v = field.voxelSize[level];
  const float fx = std::floor((position.x - field.origin.x) / v);
  const float fz = std::floor((position.z - field.origin.z) / v);
  const auto voxelsX = static_cast<float>(field.cellsX * farColumnsPerCell(level) * kBrickEdge);
  const auto voxelsZ = static_cast<float>(field.cellsZ * farColumnsPerCell(level) * kBrickEdge);
  if (fx < 0.0f || fz < 0.0f || fx >= voxelsX || fz >= voxelsZ) {
    return 0.0f;
  }
  const auto i = static_cast<int32_t>(fx), j = static_cast<int32_t>(fz);
  const BrickColumn &column =
      field.columns[level][farColumnIndex(field.cellsX, level, i / 4, j / 4)];
  const float base = field.heightBase + columnBaseHeight(column) * kFarHeightUnit;
  const float k = std::floor((position.y - base) / v);
  if (k < 0.0f || k >= static_cast<float>(columnBrickCount(column) * kBrickEdge)) {
    return 0.0f;
  }
  return unpackVoxel(voxelAt(field, level, column, i, static_cast<int32_t>(k), j)).extinction;
}

namespace {

// The optical depth over [t0, t1] inside global voxel column (i, j):
// a DDA over its layers.
double voxelColumnDepth(const FarFieldCpu &field, uint32_t level, const BrickColumn &column,
                        int32_t i, int32_t j, Vec3 origin, Vec3 direction, double t0, double t1) {
  const double v = field.voxelSize[level];
  const double base =
      field.heightBase + static_cast<double>(columnBaseHeight(column)) * kFarHeightUnit;
  const double y0 = origin.y + direction.y * t0, y1 = origin.y + direction.y * t1;
  const double top = base + columnBrickCount(column) * kBrickEdge * v;
  if (std::max(y0, y1) <= base || std::min(y0, y1) >= top) {
    return 0.0;
  }
  const double fy = (y0 - base) / v;
  auto k = static_cast<int64_t>(std::floor(fy));
  if (direction.y < 0.0f && fy == static_cast<double>(k)) {
    --k;
  }
  const int64_t step = direction.y > 0.0f ? 1 : -1;
  const int64_t layers = int64_t{columnBrickCount(column)} * kBrickEdge;
  double depth = 0.0, t = t0;
  while (t < t1) {
    const double next = direction.y == 0.0f
                            ? 1e300
                            : (base + double(k + (step > 0 ? 1 : 0)) * v - origin.y) / direction.y;
    const double end = std::min(next, t1);
    if (k >= 0 && k < layers && end > t) {
      depth +=
          unpackVoxel(voxelAt(field, level, column, i, static_cast<int32_t>(k), j)).extinction *
          (end - t);
    }
    if (next >= t1 || (step > 0 ? k >= layers : k < 0)) {
      break;
    }
    k += step;
    t = std::max(t, end);
  }
  return depth;
}

// Amanatides and Woo (1987): a DDA over the level's x/z voxel lattice.
double levelDepth(const FarFieldCpu &field, uint32_t level, Vec3 origin, Vec3 direction,
                  double start, double end, const std::vector<uint8_t> *cellInTlas) {
  const double v = field.voxelSize[level];
  const uint32_t n = farColumnsPerCell(level);
  const auto voxelsX = static_cast<int64_t>(field.cellsX * n * kBrickEdge);
  const auto voxelsZ = static_cast<int64_t>(field.cellsZ * n * kBrickEdge);
  const double ox = (origin.x - field.origin.x) / v, oz = (origin.z - field.origin.z) / v;
  const double dx = direction.x / v, dz = direction.z / v;
  const auto cellOf = [](double coordinate, double d) {
    const double f = std::floor(coordinate);
    return static_cast<int64_t>(d < 0.0 && coordinate == f ? f - 1.0 : f);
  };
  int64_t i = cellOf(ox + dx * start, dx), j = cellOf(oz + dz * start, dz);
  const int64_t si = dx > 0.0 ? 1 : -1, sj = dz > 0.0 ? 1 : -1;
  const auto boundary = [](double o, double d, int64_t index) {
    return d == 0.0 ? 1e300 : (double(index + (d > 0.0 ? 1 : 0)) - o) / d;
  };
  double depth = 0.0, t = start;
  while (t < end) {
    const double tx = boundary(ox, dx, i), tz = boundary(oz, dz, j);
    const double t1 = std::min(std::min(tx, tz), end);
    if (t1 > t && i >= 0 && j >= 0 && i < voxelsX && j < voxelsZ) {
      const size_t c = farColumnIndex(field.cellsX, level, static_cast<uint32_t>(i / 4),
                                      static_cast<uint32_t>(j / 4));
      const size_t cell = c / (n * n);
      const BrickColumn &column = field.columns[level][c];
      if (columnBrickCount(column) > 0 && !(cellInTlas && (*cellInTlas)[cell])) {
        depth += voxelColumnDepth(field, level, column, static_cast<int32_t>(i),
                                  static_cast<int32_t>(j), origin, direction, t, t1);
      }
    }
    if (t1 >= end) {
      break;
    }
    if (tx <= tz) {
      i += si;
    }
    if (tz <= tx) {
      j += sj;
    }
    t = std::max(t, t1);
  }
  return depth;
}

} // namespace

float farFieldTransmittanceCpu(const FarFieldCpu &field, Vec3 origin, Vec3 direction, float start,
                               float max, const std::vector<uint8_t> *cellInTlas) {
  double depth = 0.0;
  double levelStart = 0.0;
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    const double levelEnd = level + 1 < kFarLevels ? kFarRayLevelEnd[level] : 1e300;
    const double a = std::max<double>(start, levelStart), b = std::min<double>(max, levelEnd);
    if (a < b && !field.columns[level].empty()) {
      depth += levelDepth(field, level, origin, direction, a, b, cellInTlas);
    }
    levelStart = levelEnd;
  }
  return static_cast<float>(std::exp(-depth));
}

} // namespace meridian
