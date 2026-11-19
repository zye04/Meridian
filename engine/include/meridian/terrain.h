#pragma once

#include <meridian/mesh.h>

#include <cstdint>

namespace meridian {

struct TerrainSettings {
  int resolution = 512;
  float size = 2048.0f;
  float maxHeight = 250.0f;
  uint32_t seed = 1;
};

// A square grid centered on the origin, shaped by fractal value noise.
MeshData generateTerrain(const TerrainSettings &settings);

} // namespace meridian
