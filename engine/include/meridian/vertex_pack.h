#pragma once

#include <meridian/mesh.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace meridian {

// The GPU vertex: 20 bytes, quantized per mesh. Must match PackedVertex and
// loadVertex in common.glsl.
//   0: position x, y (snorm16)        4: position z (snorm16), COLOR_0 alpha (unorm16)
//   8: normal, octahedral 2 x snorm16 12: tangent, octahedral 2 x 15 bit, bit 31 = bitangent sign < 0
//  16: uv, 2 x unorm16 over the mesh's uv range
// The first 8 bytes read as R16G16B16A16_SNORM for acceleration structure
// builds (w ignored), with the dequantization as the geometry's transform.
struct PackedVertex {
  uint32_t words[5];
};
static_assert(sizeof(PackedVertex) == 20);

// position = center + halfExtent * snorm; uv = uvOffset + uvScale * unorm.
struct VertexQuantization {
  Vec3 center;
  Vec3 halfExtent;
  float uvOffset[2] = {0.0f, 0.0f};
  float uvScale[2] = {1.0f, 1.0f};
};

inline VertexQuantization vertexQuantization(std::span<const Vertex> vertices) {
  VertexQuantization q;
  if (vertices.empty()) {
    return q;
  }
  Vec3 lo = vertices.front().position, hi = lo;
  float uvLo[2] = {vertices.front().uvX, vertices.front().uvY};
  float uvHi[2] = {uvLo[0], uvLo[1]};
  for (const Vertex &v : vertices) {
    lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
    hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
    const float uv[2] = {v.uvX, v.uvY};
    for (int k = 0; k < 2; ++k) {
      uvLo[k] = std::min(uvLo[k], uv[k]);
      uvHi[k] = std::max(uvHi[k], uv[k]);
    }
  }
  q.center = (lo + hi) * 0.5f;
  q.halfExtent = (hi - lo) * 0.5f;
  for (int k = 0; k < 2; ++k) {
    q.uvOffset[k] = uvLo[k];
    q.uvScale[k] = uvHi[k] > uvLo[k] ? uvHi[k] - uvLo[k] : 1.0f;
  }
  return q;
}

namespace vertex_pack_detail {

inline uint32_t snorm16(float v) {
  return uint32_t(int32_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))) & 0xFFFFu;
}
inline float fromSnorm16(uint32_t bits) {
  return std::max(float(int16_t(uint16_t(bits))) / 32767.0f, -1.0f);
}
inline uint32_t unorm(float v, int bits) {
  const float max = float((1u << bits) - 1u);
  return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * max));
}
inline float fromUnorm(uint32_t v, int bits) { return float(v) / float((1u << bits) - 1u); }
inline float sign(float v) { return v >= 0.0f ? 1.0f : -1.0f; }

// Octahedral map of a unit vector to [-1, 1]^2.
inline void octahedral(Vec3 n, float &x, float &y) {
  const float s = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
  x = s > 0.0f ? n.x / s : 0.0f;
  y = s > 0.0f ? n.y / s : 0.0f;
  if (n.z < 0.0f) {
    const float ox = (1.0f - std::abs(y)) * sign(x), oy = (1.0f - std::abs(x)) * sign(y);
    x = ox, y = oy;
  }
}
inline Vec3 fromOctahedral(float x, float y) {
  const float z = 1.0f - std::abs(x) - std::abs(y);
  if (z < 0.0f) {
    const float ox = (1.0f - std::abs(y)) * sign(x), oy = (1.0f - std::abs(x)) * sign(y);
    x = ox, y = oy;
  }
  return normalize(Vec3{x, y, z});
}

} // namespace vertex_pack_detail

inline PackedVertex packVertex(const Vertex &v, const VertexQuantization &q) {
  using namespace vertex_pack_detail;
  const auto axis = [](float p, float c, float h) { return h > 0.0f ? (p - c) / h : 0.0f; };
  PackedVertex out;
  out.words[0] = snorm16(axis(v.position.x, q.center.x, q.halfExtent.x)) |
                 snorm16(axis(v.position.y, q.center.y, q.halfExtent.y)) << 16;
  out.words[1] = snorm16(axis(v.position.z, q.center.z, q.halfExtent.z)) | unorm(v.alpha, 16) << 16;
  float x, y;
  octahedral(v.normal, x, y);
  out.words[2] = snorm16(x) | snorm16(y) << 16;
  octahedral({v.tangent[0], v.tangent[1], v.tangent[2]}, x, y);
  out.words[3] = unorm(x * 0.5f + 0.5f, 15) | unorm(y * 0.5f + 0.5f, 15) << 15 |
                 (v.tangent[3] < 0.0f ? 1u << 31 : 0u);
  out.words[4] = unorm((v.uvX - q.uvOffset[0]) / q.uvScale[0], 16) |
                 unorm((v.uvY - q.uvOffset[1]) / q.uvScale[1], 16) << 16;
  return out;
}

// The CPU mirror of common.glsl's loadVertex.
inline Vertex unpackVertex(const PackedVertex &p, const VertexQuantization &q) {
  using namespace vertex_pack_detail;
  Vertex v;
  v.position = {q.center.x + q.halfExtent.x * fromSnorm16(p.words[0]),
                q.center.y + q.halfExtent.y * fromSnorm16(p.words[0] >> 16),
                q.center.z + q.halfExtent.z * fromSnorm16(p.words[1])};
  v.alpha = fromUnorm(p.words[1] >> 16, 16);
  v.normal = fromOctahedral(fromSnorm16(p.words[2]), fromSnorm16(p.words[2] >> 16));
  const Vec3 t = fromOctahedral(fromUnorm(p.words[3] & 0x7FFFu, 15) * 2.0f - 1.0f,
                                fromUnorm((p.words[3] >> 15) & 0x7FFFu, 15) * 2.0f - 1.0f);
  v.tangent[0] = t.x, v.tangent[1] = t.y, v.tangent[2] = t.z;
  v.tangent[3] = p.words[3] >> 31 ? -1.0f : 1.0f;
  v.uvX = q.uvOffset[0] + q.uvScale[0] * fromUnorm(p.words[4] & 0xFFFFu, 16);
  v.uvY = q.uvOffset[1] + q.uvScale[1] * fromUnorm(p.words[4] >> 16, 16);
  return v;
}

} // namespace meridian
