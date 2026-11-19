#pragma once

#include <meridian/mesh.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace meridian {

struct TerrainSettings {
  int resolution = 512;
  float size = 2048.0f;
  float maxHeight = 250.0f;
  uint32_t seed = 1;
};

// A square grid centered on the origin, shaped by fractal value noise.
MeshData generateTerrain(const TerrainSettings &settings);

// Height of the generated mesh's surface at (x, z), for placing things on
// the ground: exact at vertices, bilinear in between.
float terrainHeight(const TerrainSettings &settings, float x, float z);

// A regular grid of heights centered on the origin: east = +x, north = -z,
// sample (i, j) at x = (i - (width-1)/2) * spacing and
// z = (j - (height-1)/2) * spacing, row 0 = north edge. Heights are 16-bit
// steps: minHeight + sample * heightScale.
struct Heightfield {
  uint32_t width = 0;
  uint32_t height = 0;
  float spacing = 2.0f;
  float minHeight = 0.0f;
  float heightScale = 1.0f;
  std::vector<uint16_t> samples;

  // Height of sample (i, j), any integers: indices mirror outside the grid
  // like Vulkan's MIRRORED_REPEAT (edge sample duplicated, period 2*width).
  float sample(int64_t i, int64_t j) const;
  // Bilinear height at world (x, z), same mirroring (matches the GPU).
  float heightAt(float x, float z) const;
};

struct HeightfieldSettings { // procedural fallback
  uint32_t resolution = 2048;
  float spacing = 4.0f;
  float baseHeight = 500.0f;
  float maxHeight = 600.0f; // relief above baseHeight
  uint32_t seed = 1;
};

Heightfield generateHeightfield(const HeightfieldSettings &settings);
// Throws on a missing or malformed file.
Heightfield loadHeightfield(const std::filesystem::path &path);
void saveHeightfield(const Heightfield &field, const std::filesystem::path &path);
// Row-major heights in meters; minHeight and heightScale come from the data.
Heightfield quantizeHeightfield(std::span<const float> heights, uint32_t width,
                                uint32_t height, float spacing);

// Min/max pyramid for culling bounds and the shadow march. Level 0 is the
// samples; level k+1 texel (x, z) covers level-k texels 2x..2x+1, 2z..2z+1
// (clamped to the level). Level sizes are ceil(previous / 2), down to 1x1.
struct HeightPyramidLevel {
  uint32_t width, height;
  std::vector<uint32_t> minMax; // min | max << 16
};
std::vector<HeightPyramidLevel> buildHeightPyramid(const Heightfield &field);

} // namespace meridian
