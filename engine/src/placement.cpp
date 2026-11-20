#include <meridian/placement.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace meridian {

// The integer core below is mirrored line for line by shaders/placement.glsl;
// change both together.
namespace {

constexpr uint32_t kLayerSalt = 0x6c617972u;
constexpr uint32_t kMaskSalt = 0x6d61736bu;
constexpr uint32_t kClusterSalt = 0x636c7374u;
constexpr uint32_t kMaskOctaves = 4;
constexpr uint32_t kClusterOctaves = 2;
constexpr int32_t kOpen = 1 << 30;
constexpr int32_t kMaxCell = 256;

uint32_t mul16(uint32_t a, uint32_t b) { return (a * b + 0xFFFFu) >> 16; }

uint32_t smooth16(uint32_t p) {
  const uint32_t p2 = (p * p) >> 16;
  return std::min(3 * p2 - 2 * ((p2 * p) >> 16), 0xFFFFu);
}

uint32_t ramp16(int32_t v, int32_t lo, int32_t hi) {
  if (v <= lo) {
    return 0;
  }
  if (v >= hi) {
    return 0xFFFFu;
  }
  const uint32_t d = static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo);
  const uint32_t n = static_cast<uint32_t>(v) - static_cast<uint32_t>(lo);
  const int s = std::max(0, std::bit_width(d) - 16);
  return (n >> s) * 0xFFFFu / (d >> s);
}

uint32_t band16(int32_t v, const int32_t t[4]) {
  return mul16(smooth16(ramp16(v, t[0], t[1])), 0xFFFFu - smooth16(ramp16(v, t[2], t[3])));
}

uint32_t lerp16(uint32_t a, uint32_t b, uint32_t t) {
  return ((a * (0x10000u - t)) >> 16) + ((b * t) >> 16);
}

uint32_t valueNoise16(uint32_t u, uint32_t v, uint32_t period, uint32_t salt) {
  const int s = std::max(0, std::bit_width(period) - 15);
  const uint32_t ix = u / period;
  const uint32_t iz = v / period;
  // (period >> s) truncates, so the fraction can reach 65536 near a lattice line.
  const uint32_t fx = smooth16(std::min((((u % period) >> s) << 16) / (period >> s), 0xFFFFu));
  const uint32_t fz = smooth16(std::min((((v % period) >> s) << 16) / (period >> s), 0xFFFFu));
  const uint32_t top = lerp16(placementHash(ix, iz, salt) >> 16,
                              placementHash(ix + 1, iz, salt) >> 16, fx);
  const uint32_t bottom = lerp16(placementHash(ix, iz + 1, salt) >> 16,
                                 placementHash(ix + 1, iz + 1, salt) >> 16, fx);
  return lerp16(top, bottom, fz);
}

uint32_t fbm16(uint32_t u, uint32_t v, uint32_t period, uint32_t octaves, uint32_t salt) {
  uint32_t sum = 0;
  for (uint32_t k = 0; k < octaves; ++k) {
    sum += valueNoise16(u, v, period >> k, salt + k) << (octaves - 1 - k);
  }
  return sum / ((1u << octaves) - 1);
}

struct Factors {
  uint32_t density, mask;
};

// h: the nearest sample; dx, dz: its central differences, in sample steps.
// The mask uses the global seed (layers share it, so complements line up);
// clustering uses the layer seed (layers clump in different places).
Factors densityCore(const PlacementRule &rule, uint32_t seed, uint32_t layerSeed, int32_t x,
                    int32_t z, int32_t h, int32_t dx, int32_t dz) {
  dx = std::clamp(dx, -32767, 32767);
  dz = std::clamp(dz, -32767, 32767);
  const int32_t g2 = dx * dx + dz * dz;
  uint32_t density = mul16(band16(g2, rule.slope), band16(h, rule.altitude));
  const uint32_t u = static_cast<uint32_t>(x) ^ 0x80000000u;
  const uint32_t v = static_cast<uint32_t>(z) ^ 0x80000000u;
  uint32_t mask = 0xFFFFu;
  if (rule.maskPeriod != 0) {
    const int32_t m =
        static_cast<int32_t>(fbm16(u, v, rule.maskPeriod, kMaskOctaves, seed ^ kMaskSalt));
    mask = rule.maskLo <= rule.maskHi ? smooth16(ramp16(m, rule.maskLo, rule.maskHi))
                                      : 0xFFFFu - smooth16(ramp16(m, rule.maskHi, rule.maskLo));
  }
  density = mul16(density, mask);
  if (rule.clusterPeriod != 0) {
    const uint32_t c =
        smooth16(fbm16(u, v, rule.clusterPeriod, kClusterOctaves, layerSeed ^ kClusterSalt));
    density = mul16(density, 0xFFFFu - mul16(rule.clusterAmount, 0xFFFFu - c));
  }
  return {density, mask};
}

uint32_t layerSeed(uint32_t layerId, uint32_t seed) {
  return placementHash(layerId, seed, kLayerSalt);
}

uint32_t cellHash(int32_t cellX, int32_t cellZ, uint32_t layerSeed) {
  return placementHash(static_cast<uint32_t>(cellX), static_cast<uint32_t>(cellZ), layerSeed);
}

// The pattern point moved into the cell: one of the 8 square symmetries plus
// a toroidal offset per cell, so the tile never repeats visibly.
void pointPosition(int32_t cellX, int32_t cellZ, uint32_t cellHash, uint32_t packed,
                   int32_t &x, int32_t &z) {
  const uint32_t symmetry = placementHash(cellHash, 0, 1) & 7;
  uint32_t px = packed & 0xFFFFu;
  uint32_t pz = packed >> 16;
  if (symmetry & 4) {
    std::swap(px, pz);
  }
  px ^= (symmetry & 1) ? 0xFFFFu : 0;
  pz ^= (symmetry & 2) ? 0xFFFFu : 0;
  px = (px + cellHash) & 0xFFFFu;
  pz = (pz + (cellHash >> 16)) & 0xFFFFu;
  x = cellX * (1 << 22) + static_cast<int32_t>(px << 6); // in range: cellInRange
  z = cellZ * (1 << 22) + static_cast<int32_t>(pz << 6);
}

// Crops are under 2^31 fixed wide (placementField), so |x| < 2^30 (16 km):
// cells -256..255. Checking first keeps the cell arithmetic from overflowing.
bool cellInRange(int32_t cellX, int32_t cellZ) {
  return cellX >= -kMaxCell && cellX < kMaxCell && cellZ >= -kMaxCell && cellZ < kMaxCell;
}

bool insideCrop(const PlacementField &field, int32_t x, int32_t z) {
  return x >= field.originX && x <= -field.originX && z >= field.originZ &&
         z <= -field.originZ;
}

uint32_t nearestIndex(int32_t x, int32_t origin, uint32_t spacing, uint32_t size) {
  x = std::clamp(x, origin, -origin);
  const uint32_t i =
      (static_cast<uint32_t>(x) - static_cast<uint32_t>(origin) + spacing / 2) / spacing;
  return std::min(i, size - 1);
}

Factors densityAtFixed(const Heightfield &heights, const PlacementField &field,
                       const PlacementRule &rule, uint32_t seed, uint32_t layerSeed, int32_t x,
                       int32_t z) {
  const uint32_t i = nearestIndex(x, field.originX, field.spacing, field.width);
  const uint32_t j = nearestIndex(z, field.originZ, field.spacing, field.height);
  const auto at = [&](uint32_t a, uint32_t b) {
    return static_cast<int32_t>(heights.samples[static_cast<size_t>(b) * field.width + a]);
  };
  const uint32_t i0 = i > 0 ? i - 1 : 0, i1 = std::min(i + 1, field.width - 1);
  const uint32_t j0 = j > 0 ? j - 1 : 0, j1 = std::min(j + 1, field.height - 1);
  return densityCore(rule, seed, layerSeed, x, z, at(i, j), at(i1, j) - at(i0, j),
                     at(i, j1) - at(i, j0));
}

// IEEE half, round to nearest even; scales are positive and far from the
// half range's ends, but subnormals and overflow still come out right.
uint16_t floatToHalf(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t magnitude = bits & 0x7FFFFFFFu;
  if (magnitude >= 0x47800000u) { // >= 65536 (or inf/nan): infinity
    return static_cast<uint16_t>(sign | 0x7C00u);
  }
  if (magnitude < 0x38800000u) { // below the smallest normal half
    const uint32_t mantissa = (magnitude & 0x7FFFFFu) | 0x800000u;
    const int shift = 126 - static_cast<int>(magnitude >> 23);
    if (shift > 24) {
      return static_cast<uint16_t>(sign);
    }
    const uint32_t half = mantissa >> shift;
    const uint32_t rest = mantissa & ((1u << shift) - 1);
    const uint32_t halfway = 1u << (shift - 1);
    return static_cast<uint16_t>(
        sign | (half + (rest > halfway || (rest == halfway && (half & 1)))));
  }
  const uint32_t rounded = magnitude - 0x38000000u + 0xFFFu + ((magnitude >> 13) & 1);
  return static_cast<uint16_t>(sign | std::min(rounded >> 13, 0x7C00u));
}

float halfToFloat(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
  const uint32_t exponent = (half >> 10) & 0x1Fu;
  const uint32_t mantissa = half & 0x3FFu;
  float magnitude = 0.0f;
  if (exponent == 0) {
    magnitude = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    magnitude = mantissa ? NAN : INFINITY;
  } else {
    magnitude = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
  }
  return sign ? -magnitude : magnitude;
}

int16_t toSnorm16(float v) {
  return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

int64_t torusDistance2(uint32_t a, uint32_t b) {
  const int64_t dx = (a - b) & 0xFFFFu;
  const int64_t dz = ((a >> 16) - (b >> 16)) & 0xFFFFu;
  const int64_t wx = std::min(dx, 65536 - dx);
  const int64_t wz = std::min(dz, 65536 - dz);
  return wx * wx + wz * wz;
}

} // namespace

uint32_t placementHash(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t x = a * 1664525u + 1013904223u;
  uint32_t y = b * 1664525u + 1013904223u;
  uint32_t z = c * 1664525u + 1013904223u;
  x += y * z;
  y += z * x;
  z += x * y;
  x ^= x >> 16;
  y ^= y >> 16;
  z ^= z >> 16;
  x += y * z;
  return x;
}

// Mitchell's best-candidate sampling on the torus: each new point is the
// candidate farthest from those before it, with candidates growing with the
// point count, so every prefix is a blue-noise set and its rank is the
// threshold.
DitherPattern makeDitherPattern(uint32_t seed, uint32_t count) {
  constexpr uint32_t kCandidatesPerPoint = 8;
  // Best-candidate sampling is quadratic in the point count; past this the
  // dense (streamed) layers' patterns would take minutes to build.
  constexpr uint32_t kMaxCandidates = 512;
  DitherPattern pattern;
  pattern.points.reserve(count);
  pattern.thresholds.reserve(count);
  const uint32_t grid = std::max(1u, static_cast<uint32_t>(std::sqrt(static_cast<double>(count))));
  const int64_t width = 65536 / grid;
  std::vector<std::vector<uint32_t>> buckets(static_cast<size_t>(grid) * grid);
  std::vector<uint32_t> stamp(buckets.size(), 0);
  uint32_t query = 0;
  const auto bucketOf = [&](uint32_t p) {
    return std::pair<int64_t, int64_t>(((p & 0xFFFFu) * grid) >> 16, ((p >> 16) * grid) >> 16);
  };
  const auto nearest2 = [&](uint32_t c) {
    ++query;
    const auto [bx, bz] = bucketOf(c);
    int64_t best = INT64_MAX;
    for (int64_t r = 0; r <= grid / 2; ++r) {
      if (r > 0 && (r - 1) * width * (r - 1) * width >= best) {
        break;
      }
      for (int64_t dz = -r; dz <= r; ++dz) {
        for (int64_t dx = -r; dx <= r; ++dx) {
          if (std::max(std::abs(dx), std::abs(dz)) != r) {
            continue;
          }
          const size_t b = static_cast<size_t>(((bz + dz + grid) % grid) * grid +
                                               (bx + dx + grid) % grid);
          if (stamp[b] == query) {
            continue;
          }
          stamp[b] = query;
          for (uint32_t p : buckets[b]) {
            best = std::min(best, torusDistance2(c, pattern.points[p]));
          }
        }
      }
    }
    return best;
  };
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t chosen = 0;
    int64_t chosenDistance = -1;
    for (uint32_t k = 0; k <= std::min(kCandidatesPerPoint * i, kMaxCandidates); ++k) {
      const uint32_t candidate = placementHash(seed, i, k);
      const int64_t d = nearest2(candidate);
      if (d > chosenDistance) {
        chosen = candidate;
        chosenDistance = d;
      }
    }
    const auto [bx, bz] = bucketOf(chosen);
    buckets[static_cast<size_t>(bz * grid + bx)].push_back(i);
    pattern.points.push_back(chosen);
    pattern.thresholds.push_back(static_cast<uint16_t>((uint64_t{i} << 16) / count));
  }
  return pattern;
}

uint32_t ditherPointCount(float footprint) {
  const double side = kCellSize / footprint;
  return static_cast<uint32_t>(std::max(1LL, std::llround(side * side)));
}

PlacementField placementField(const Heightfield &heights) {
  const auto spacing = static_cast<uint32_t>(std::llround(heights.spacing * 65536.0));
  const uint64_t extentX = uint64_t{heights.width - 1} * spacing;
  const uint64_t extentZ = uint64_t{heights.height - 1} * spacing;
  if (extentX >= (1ull << 31) || extentZ >= (1ull << 31)) {
    throw std::runtime_error("heightfield too large for fixed-point placement");
  }
  return {-static_cast<int32_t>(extentX / 2), -static_cast<int32_t>(extentZ / 2), spacing,
          heights.width, heights.height};
}

PlacementRule placementRule(const Heightfield &heights, const DensityRule &rule) {
  PlacementRule out{};
  const auto clampToInt = [](double v, double lo, double hi) {
    return static_cast<int32_t>(std::llround(std::clamp(v, lo, hi)));
  };

  const double stepsPerTan = 2.0 * heights.spacing / heights.heightScale;
  const auto slopeSteps = [&](double deg) {
    if (deg >= 90.0) {
      return INT32_MAX;
    }
    const double t = std::tan(std::max(deg, 0.0) * std::numbers::pi / 180.0) * stepsPerTan;
    return clampToInt(t * t, 0.0, INT32_MAX);
  };
  const double slopeLo = std::max<double>(rule.slopeMinDeg, 0.0);
  const double slopeHi = std::min<double>(rule.slopeMaxDeg, 90.0);
  const double slopeFade = std::max(0.0, rule.fade * (slopeHi - slopeLo));
  const bool slopeLowOpen = rule.slopeMinDeg <= 0.0f;
  const bool slopeHighOpen = rule.slopeMaxDeg >= 90.0f;
  out.slope[0] = slopeLowOpen ? -1 : slopeSteps(slopeLo);
  out.slope[1] = slopeLowOpen ? -1 : slopeSteps(slopeLo + slopeFade);
  out.slope[2] = slopeHighOpen ? INT32_MAX : slopeSteps(slopeHi - slopeFade);
  out.slope[3] = slopeHighOpen ? INT32_MAX : slopeSteps(slopeHi);

  const double fieldMax = heights.minHeight + 65535.0 * heights.heightScale;
  const auto altitudeSteps = [&](double meters) {
    return clampToInt((meters - heights.minHeight) / heights.heightScale, -kOpen, kOpen);
  };
  const double altitudeLo = std::max<double>(rule.altitudeMin, heights.minHeight);
  const double altitudeHi = std::min<double>(rule.altitudeMax, fieldMax);
  const double altitudeFade = std::max(0.0, rule.fade * (altitudeHi - altitudeLo));
  const bool altitudeLowOpen = rule.altitudeMin <= heights.minHeight;
  const bool altitudeHighOpen = rule.altitudeMax >= fieldMax;
  out.altitude[0] = altitudeLowOpen ? -kOpen : altitudeSteps(rule.altitudeMin);
  out.altitude[1] = altitudeLowOpen ? -kOpen : altitudeSteps(rule.altitudeMin + altitudeFade);
  out.altitude[2] = altitudeHighOpen ? kOpen : altitudeSteps(rule.altitudeMax - altitudeFade);
  out.altitude[3] = altitudeHighOpen ? kOpen : altitudeSteps(rule.altitudeMax);

  const auto period = [](float frequency) {
    return static_cast<uint32_t>(
        std::llround(std::clamp(65536.0 / frequency, 65536.0, 2147483648.0)));
  };
  if (rule.maskFrequency > 0.0f) {
    out.maskPeriod = period(rule.maskFrequency);
    out.maskLo = clampToInt(rule.maskThreshold * 65535.0, -1.0, 65536.0);
    out.maskHi =
        clampToInt((rule.maskThreshold + std::abs(rule.maskEdge)) * 65535.0, -1.0, 65536.0);
    if (rule.maskEdge < 0.0f) {
      std::swap(out.maskLo, out.maskHi);
    }
  }
  if (rule.clusterFrequency > 0.0f && rule.clusterAmount > 0.0f) {
    out.clusterPeriod = period(rule.clusterFrequency);
    out.clusterAmount = static_cast<uint32_t>(clampToInt(rule.clusterAmount * 65535.0, 0, 65535));
  }
  return out;
}

std::vector<uint32_t> prototypeThresholds(const PlacementLayer &layer) {
  double total = 0.0;
  for (const auto &[prototype, weight] : layer.prototypes) {
    total += weight;
  }
  if (!(total > 0.0)) {
    throw std::runtime_error("placement layer needs a positive total prototype weight");
  }
  std::vector<uint32_t> out;
  double sum = 0.0;
  for (const auto &[prototype, weight] : layer.prototypes) {
    sum += weight;
    out.push_back(static_cast<uint32_t>(std::llround(65536.0 * sum / total)));
  }
  if (!out.empty()) {
    out.back() = 65536;
  }
  return out;
}

uint16_t densityAt(const Heightfield &field, const PlacementLayer &layer, uint32_t seed,
                   int64_t xFixed, int64_t zFixed) {
  const auto narrow = [](int64_t v) {
    return static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
  };
  return static_cast<uint16_t>(densityAtFixed(field, placementField(field),
                                              placementRule(field, layer.rule), seed,
                                              layerSeed(layer.id, seed), narrow(xFixed),
                                              narrow(zFixed))
                                   .density);
}

std::optional<PlacedPoint> evaluatePoint(const Heightfield &field, const PlacementLayer &layer,
                                         const DitherPattern &pattern, uint32_t seed,
                                         int32_t cellX, int32_t cellZ, uint32_t point) {
  const std::vector<uint32_t> thresholds = prototypeThresholds(layer);
  if (point >= pattern.points.size() || !cellInRange(cellX, cellZ)) {
    return std::nullopt;
  }
  const PlacementField fixedField = placementField(field);
  const uint32_t layerHash = layerSeed(layer.id, seed);
  const uint32_t cell = cellHash(cellX, cellZ, layerHash);
  int32_t x = 0, z = 0;
  pointPosition(cellX, cellZ, cell, pattern.points[point], x, z);
  if (!insideCrop(fixedField, x, z)) {
    return std::nullopt;
  }
  const Factors factors =
      densityAtFixed(field, fixedField, placementRule(field, layer.rule), seed, layerHash, x, z);
  if (pattern.thresholds[point] >= factors.density) {
    return std::nullopt;
  }

  const uint32_t pointHash = placementHash(cell, point, 2);
  size_t pick = 0;
  while ((pointHash & 0xFFFFu) >= thresholds[pick]) {
    ++pick;
  }

  constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
  const auto unit = [](uint32_t bits) { return static_cast<float>(bits & 0xFFFFu) / 65535.0f; };
  PlacedPoint out{};
  out.position.x = static_cast<float>(x) / 65536.0f;
  out.position.z = static_cast<float>(z) / 65536.0f;
  out.position.y = field.heightAt(out.position.x, out.position.z) - layer.sinkDepth;
  out.prototype = layer.prototypes[pick].first;
  const float edge = 0.5f + 0.5f * static_cast<float>(factors.mask) / 65535.0f;
  out.scale = (layer.scaleMin + (layer.scaleMax - layer.scaleMin) * unit(pointHash >> 16)) * edge;
  const uint32_t rotationHash = placementHash(pointHash, 3, 0);
  out.yaw = unit(rotationHash) * kTwoPi;
  out.axis = {0.0f, 1.0f, 0.0f};
  out.angle = out.yaw;
  if (layer.rotation == Rotation::Full) {
    const float y = 2.0f * unit(rotationHash >> 16) - 1.0f;
    const float phi = unit(placementHash(pointHash, 4, 0)) * kTwoPi;
    const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    out.axis = {r * std::cos(phi), y, r * std::sin(phi)};
    out.angle = unit(placementHash(pointHash, 5, 0)) * kTwoPi;
  }
  return out;
}

PlacementGrid placementGrid(const Heightfield &heights) {
  const PlacementField field = placementField(heights);
  // Arithmetic shifts floor: cell c holds [c, c + 1) * 2^22 (64 m).
  const auto cells = [](int32_t origin) {
    return static_cast<uint32_t>((-origin >> 22) - (origin >> 22) + 1);
  };
  const PlacementGrid grid{field.originX >> 22, field.originZ >> 22, cells(field.originX),
                           cells(field.originZ)};
  if (!cellInRange(grid.firstX, grid.firstZ) ||
      !cellInRange(grid.firstX + static_cast<int32_t>(grid.columns) - 1,
                   grid.firstZ + static_cast<int32_t>(grid.rows) - 1)) {
    throw std::runtime_error("heightfield too large for the placement cells");
  }
  return grid;
}

std::array<float, 4> placedRotation(const PlacedPoint &point) {
  const float s = std::sin(0.5f * point.angle);
  std::array<float, 4> q = {point.axis.x * s, point.axis.y * s, point.axis.z * s,
                            std::cos(0.5f * point.angle)};
  if (q[3] < 0.0f) {
    for (float &c : q) {
      c = -c;
    }
  }
  return q;
}

PlacedInstance encodePlacedInstance(const PlacedPoint &point) {
  const std::array<float, 4> q = placedRotation(point);
  PlacedInstance out{};
  out.position[0] = point.position.x;
  out.position[1] = point.position.y;
  out.position[2] = point.position.z;
  for (int i = 0; i < 3; ++i) {
    out.rotation[i] = toSnorm16(q[i]);
  }
  out.scaleHalf = floatToHalf(point.scale);
  out.prototype = point.prototype;
  return out;
}

DecodedInstance decodePlacedInstance(const PlacedInstance &instance) {
  DecodedInstance out{};
  out.position = {instance.position[0], instance.position[1], instance.position[2]};
  float length2 = 0.0f;
  for (int i = 0; i < 3; ++i) {
    out.rotation[i] = std::max(static_cast<float>(instance.rotation[i]) / 32767.0f, -1.0f);
    length2 += out.rotation[i] * out.rotation[i];
  }
  out.rotation[3] = std::sqrt(std::max(0.0f, 1.0f - length2));
  // Rounding can push |xyz| past 1 (then w = 0); renormalize.
  const float norm = std::sqrt(length2 + out.rotation[3] * out.rotation[3]);
  for (float &c : out.rotation) {
    c /= norm;
  }
  out.scale = halfToFloat(instance.scaleHalf);
  out.prototype = instance.prototype;
  return out;
}

namespace {

float smoothstep01(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

int32_t floorMod(int32_t a, int32_t m) { return ((a % m) + m) % m; }

} // namespace

float streamKeepFraction(float distance, float fullDensity, float maxDistance) {
  if (distance >= maxDistance) {
    return 0.0f;
  }
  const float ratio = distance <= fullDensity ? 1.0f : fullDensity / distance;
  const float thin = distance <= fullDensity ? 1.0f : ratio * ratio;
  return thin * (1.0f - smoothstep01(0.5f * maxDistance, maxDistance, distance));
}

float streamFade(float keepFraction, float threshold) {
  return smoothstep01(0.0f, 0.3f, keepFraction - threshold);
}

uint32_t streamWindowCells(float radius) {
  return 2u * static_cast<uint32_t>(std::ceil(radius / kCellSize)) + 1u;
}

std::pair<uint32_t, uint32_t> streamSlot(int32_t cellX, int32_t cellZ, uint32_t window) {
  const auto w = static_cast<int32_t>(window);
  return {static_cast<uint32_t>(floorMod(cellX, w)), static_cast<uint32_t>(floorMod(cellZ, w))};
}

std::pair<int32_t, int32_t> streamSlotCell(uint32_t slotX, uint32_t slotZ, int32_t cameraCellX,
                                           int32_t cameraCellZ, uint32_t window) {
  const auto w = static_cast<int32_t>(window);
  const int32_t half = w / 2;
  auto cell = [&](uint32_t slot, int32_t camera) {
    const int32_t low = camera - half;
    return low + floorMod(static_cast<int32_t>(slot) - low, w);
  };
  return {cell(slotX, cameraCellX), cell(slotZ, cameraCellZ)};
}

} // namespace meridian
