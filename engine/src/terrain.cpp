#include <meridian/terrain.h>

#include <algorithm>
#include <cmath>

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

float vertexHeight(const TerrainSettings &settings, float x, float z) {
  const float n = fractalNoise(x, z, settings.seed);
  // Squaring flattens the lowlands and sharpens the peaks.
  return n * n * settings.maxHeight;
}

} // namespace

float terrainHeight(const TerrainSettings &settings, float x, float z) {
  const float spacing = settings.size / static_cast<float>(settings.resolution);
  const float origin = -settings.size * 0.5f;
  const float gx = std::clamp((x - origin) / spacing, 0.0f,
                              static_cast<float>(settings.resolution));
  const float gz = std::clamp((z - origin) / spacing, 0.0f,
                              static_cast<float>(settings.resolution));
  const int x0 = std::min(static_cast<int>(gx), settings.resolution - 1);
  const int z0 = std::min(static_cast<int>(gz), settings.resolution - 1);
  const float fx = gx - x0;
  const float fz = gz - z0;
  auto h = [&](int ix, int iz) {
    return vertexHeight(settings, origin + ix * spacing, origin + iz * spacing);
  };
  const float top = h(x0, z0) + (h(x0 + 1, z0) - h(x0, z0)) * fx;
  const float bottom = h(x0, z0 + 1) + (h(x0 + 1, z0 + 1) - h(x0, z0 + 1)) * fx;
  return top + (bottom - top) * fz;
}

MeshData generateTerrain(const TerrainSettings &settings) {
  const int verticesPerSide = settings.resolution + 1;
  const float spacing = settings.size / static_cast<float>(settings.resolution);
  const float origin = -settings.size * 0.5f;

  std::vector<float> heights(static_cast<size_t>(verticesPerSide) *
                             verticesPerSide);
  for (int z = 0; z < verticesPerSide; ++z) {
    for (int x = 0; x < verticesPerSide; ++x) {
      heights[z * verticesPerSide + x] =
          vertexHeight(settings, origin + x * spacing, origin + z * spacing);
    }
  }

  auto heightAt = [&](int x, int z) {
    x = x < 0 ? 0 : (x >= verticesPerSide ? verticesPerSide - 1 : x);
    z = z < 0 ? 0 : (z >= verticesPerSide ? verticesPerSide - 1 : z);
    return heights[z * verticesPerSide + x];
  };

  MeshData mesh;
  mesh.vertices.reserve(heights.size());
  for (int z = 0; z < verticesPerSide; ++z) {
    for (int x = 0; x < verticesPerSide; ++x) {
      Vertex vertex;
      vertex.position = {origin + x * spacing, heightAt(x, z),
                         origin + z * spacing};
      vertex.normal = normalize({heightAt(x - 1, z) - heightAt(x + 1, z),
                                 2.0f * spacing,
                                 heightAt(x, z - 1) - heightAt(x, z + 1)});
      Vec3 tangent = normalize(
          {2.0f * spacing, heightAt(x + 1, z) - heightAt(x - 1, z), 0.0f});
      vertex.tangent[0] = tangent.x;
      vertex.tangent[1] = tangent.y;
      vertex.tangent[2] = tangent.z;
      vertex.uvX = static_cast<float>(x) / settings.resolution;
      vertex.uvY = static_cast<float>(z) / settings.resolution;
      mesh.vertices.push_back(vertex);
    }
  }

  mesh.indices.reserve(static_cast<size_t>(settings.resolution) *
                       settings.resolution * 6);
  for (int z = 0; z < settings.resolution; ++z) {
    for (int x = 0; x < settings.resolution; ++x) {
      uint32_t i0 = z * verticesPerSide + x;
      uint32_t i1 = i0 + 1;
      uint32_t i2 = i0 + verticesPerSide;
      uint32_t i3 = i2 + 1;
      // Counter-clockwise when seen from above.
      mesh.indices.insert(mesh.indices.end(), {i0, i2, i1, i1, i2, i3});
    }
  }

  return mesh;
}

} // namespace meridian
