#pragma once

#include <meridian/math/vec3.h>

#include <cstdint>
#include <vector>

namespace meridian {

// Layout matches the std430 Vertex struct in the shaders: each vec3 is padded
// to 16 bytes, so the UVs fill the padding for free.
struct Vertex {
  Vec3 position;
  float uvX = 0.0f;
  Vec3 normal;
  float uvY = 0.0f;
  // xyz: tangent along +U; w: bitangent sign (glTF/MikkTSpace convention).
  float tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
};
static_assert(sizeof(Vertex) == 48);

struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
};

} // namespace meridian
