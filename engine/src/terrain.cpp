#include <meridian/terrain.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace meridian {

namespace {

float hash(int x, int z, uint32_t seed) {
  uint32_t h = seed + static_cast<uint32_t>(x) * 374761393u +
               static_cast<uint32_t>(z) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

float valueNoise(float x, float z, uint32_t seed) {
  float fx = std::floor(x);
  float fz = std::floor(z);
  int ix = static_cast<int>(fx);
  int iz = static_cast<int>(fz);
  float tx = smoothstep(x - fx);
  float tz = smoothstep(z - fz);

  float a = hash(ix, iz, seed);
  float b = hash(ix + 1, iz, seed);
  float c = hash(ix, iz + 1, seed);
  float d = hash(ix + 1, iz + 1, seed);
  float top = a + (b - a) * tx;
  float bottom = c + (d - c) * tx;
  return top + (bottom - top) * tz;
}

// Fractal sum of noise octaves, normalized to [0, 1].
float fractalNoise(float x, float z, uint32_t seed) {
  constexpr int kOctaves = 7;
  float sum = 0.0f;
  float amplitude = 1.0f;
  float frequency = 1.0f / 512.0f;
  float total = 0.0f;
  for (int i = 0; i < kOctaves; ++i) {
    sum += valueNoise(x * frequency, z * frequency, seed + i) * amplitude;
    total += amplitude;
    amplitude *= 0.5f;
    frequency *= 2.0f;
  }
  return sum / total;
}

int64_t mirrorIndex(int64_t i, int64_t size) {
  const int64_t m = ((i % (2 * size)) + 2 * size) % (2 * size);
  return m < size ? m : 2 * size - 1 - m;
}

struct HeightfieldHeader {
  char magic[4];
  uint32_t version;
  uint32_t width;
  uint32_t height;
  float spacing;
  float minHeight;
  float heightScale;
};
static_assert(sizeof(HeightfieldHeader) == 28);

} // namespace

float Heightfield::sample(int64_t i, int64_t j) const {
  const int64_t x = mirrorIndex(i, width);
  const int64_t y = mirrorIndex(j, height);
  return minHeight + heightScale * samples[static_cast<size_t>(y) * width + x];
}

float Heightfield::heightAt(float x, float z) const {
  const float gx = x / spacing + 0.5f * static_cast<float>(width - 1);
  const float gz = z / spacing + 0.5f * static_cast<float>(height - 1);
  const float fx0 = std::floor(gx);
  const float fz0 = std::floor(gz);
  const int64_t i = static_cast<int64_t>(fx0);
  const int64_t j = static_cast<int64_t>(fz0);
  const float tx = gx - fx0;
  const float tz = gz - fz0;
  const float top = sample(i, j) + (sample(i + 1, j) - sample(i, j)) * tx;
  const float bottom =
      sample(i, j + 1) + (sample(i + 1, j + 1) - sample(i, j + 1)) * tx;
  return top + (bottom - top) * tz;
}

Heightfield quantizeHeightfield(std::span<const float> heights, uint32_t width,
                                uint32_t height, float spacing) {
  if (heights.size() != static_cast<size_t>(width) * height || heights.empty()) {
    throw std::runtime_error("heightfield size does not match its data");
  }
  const auto [lo, hi] = std::minmax_element(heights.begin(), heights.end());
  Heightfield field;
  field.width = width;
  field.height = height;
  field.spacing = spacing;
  field.minHeight = *lo;
  field.heightScale = *hi > *lo ? (*hi - *lo) / 65535.0f : 1.0f;
  field.samples.resize(heights.size());
  for (size_t k = 0; k < heights.size(); ++k) {
    const float step = std::round((heights[k] - field.minHeight) / field.heightScale);
    field.samples[k] = static_cast<uint16_t>(std::clamp(step, 0.0f, 65535.0f));
  }
  return field;
}

Heightfield generateHeightfield(const HeightfieldSettings &settings) {
  const uint32_t n = settings.resolution;
  const float origin = -0.5f * static_cast<float>(n - 1) * settings.spacing;
  std::vector<float> heights(static_cast<size_t>(n) * n);
  for (uint32_t j = 0; j < n; ++j) {
    for (uint32_t i = 0; i < n; ++i) {
      const float v = fractalNoise(origin + i * settings.spacing,
                                   origin + j * settings.spacing, settings.seed);
      // Squaring flattens the lowlands and sharpens the peaks.
      heights[static_cast<size_t>(j) * n + i] =
          settings.baseHeight + v * v * settings.maxHeight;
    }
  }
  return quantizeHeightfield(heights, n, n, settings.spacing);
}

void saveHeightfield(const Heightfield &field, const std::filesystem::path &path) {
  HeightfieldHeader header{{'M', 'H', 'F', '1'}, 1, field.width, field.height,
                           field.spacing, field.minHeight, field.heightScale};
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char *>(&header), sizeof(header));
  stream.write(reinterpret_cast<const char *>(field.samples.data()),
               static_cast<std::streamsize>(field.samples.size() * sizeof(uint16_t)));
  if (!stream) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

Heightfield loadHeightfield(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open " + path.string());
  }
  HeightfieldHeader header;
  stream.read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!stream || std::memcmp(header.magic, "MHF1", 4) != 0 || header.version != 1 ||
      header.width == 0 || header.height == 0) {
    throw std::runtime_error("not a heightfield file: " + path.string());
  }
  const uint64_t expected =
      sizeof(header) + static_cast<uint64_t>(header.width) * header.height * 2;
  if (std::filesystem::file_size(path) != expected) {
    throw std::runtime_error("heightfield file has the wrong size: " + path.string());
  }
  Heightfield field;
  field.width = header.width;
  field.height = header.height;
  field.spacing = header.spacing;
  field.minHeight = header.minHeight;
  field.heightScale = header.heightScale;
  field.samples.resize(static_cast<size_t>(header.width) * header.height);
  stream.read(reinterpret_cast<char *>(field.samples.data()),
              static_cast<std::streamsize>(field.samples.size() * sizeof(uint16_t)));
  if (!stream) {
    throw std::runtime_error("truncated heightfield file: " + path.string());
  }
  return field;
}

std::vector<HeightPyramidLevel> buildHeightPyramid(const Heightfield &field) {
  std::vector<HeightPyramidLevel> levels;
  HeightPyramidLevel base{field.width, field.height, {}};
  base.minMax.resize(field.samples.size());
  for (size_t k = 0; k < field.samples.size(); ++k) {
    base.minMax[k] = field.samples[k] | (static_cast<uint32_t>(field.samples[k]) << 16);
  }
  levels.push_back(std::move(base));
  while (levels.back().width > 1 || levels.back().height > 1) {
    const HeightPyramidLevel &prev = levels.back();
    HeightPyramidLevel next{(prev.width + 1) / 2, (prev.height + 1) / 2, {}};
    next.minMax.resize(static_cast<size_t>(next.width) * next.height);
    for (uint32_t y = 0; y < next.height; ++y) {
      for (uint32_t x = 0; x < next.width; ++x) {
        uint32_t lo = 0xFFFF;
        uint32_t hi = 0;
        for (uint32_t dy = 0; dy < 2; ++dy) {
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sx = std::min(2 * x + dx, prev.width - 1);
            const uint32_t sy = std::min(2 * y + dy, prev.height - 1);
            const uint32_t v = prev.minMax[static_cast<size_t>(sy) * prev.width + sx];
            lo = std::min(lo, v & 0xFFFFu);
            hi = std::max(hi, v >> 16);
          }
        }
        next.minMax[static_cast<size_t>(y) * next.width + x] = lo | (hi << 16);
      }
    }
    levels.push_back(std::move(next));
  }
  return levels;
}

} // namespace meridian
