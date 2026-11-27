#pragma once

#include <meridian/math/vec3.h>

#include <cstdint>

// The visibility buffer's per-pixel id and the barycentrics the resolve
// rebuilds a triangle's pixel from. CPU reference for visibility.glsl, which
// mirrors it operation for operation.
namespace meridian {

enum class VisibilityKind : uint32_t { None = 0, ClusterTriangle = 1, Voxel = 2 };

// The draw list's terrain sun code (cluster_cull.comp: 0 shadowed, 1 lit,
// 255 marched per pixel) in two bits.
constexpr uint32_t kVisibilityTerrainSunPerPixel = 3;

// RG32UI. Cluster triangle: x = instance (24 bits) | triangle << 24 (7),
// y = cluster (26) | terrain sun << 26 (2) | kind << 30. Voxel: x = 0,
// y = index in its phase's voxel list (26) | phase << 26 | kind << 30.
// The clear value (0, 0) is kind None.
struct VisibilityId {
  uint32_t x = 0;
  uint32_t y = 0;
};

struct UnpackedVisibility {
  VisibilityKind kind = VisibilityKind::None;
  uint32_t instance = 0;
  uint32_t cluster = 0;
  uint32_t triangle = 0;
  // 0, 1 or 255, as the draw list entry carries it.
  uint32_t terrainSunCode = 0;
  uint32_t phase = 0;
  uint32_t index = 0;
};

VisibilityId packClusterTriangle(uint32_t instance, uint32_t cluster, uint32_t triangle,
                                 uint32_t terrainSunCode);
VisibilityId packVoxel(uint32_t phase, uint32_t index);
VisibilityKind visibilityKind(VisibilityId id);
UnpackedVisibility unpackVisibility(VisibilityId id);

struct Barycentrics {
  // Perspective-correct weights of the three vertices and their change per
  // pixel step in x and y (framebuffer axes).
  Vec3 b;
  Vec3 dbdx;
  Vec3 dbdy;
  // Vulkan's facing rule for VK_FRONT_FACE_COUNTER_CLOCKWISE: the signed
  // area in framebuffer coordinates is positive.
  bool frontFacing = true;
  // How far the most negative weight is below 0 (0 inside the triangle).
  float outside = 0.0f;
};

// The pixel at framebuffer position (pixelX, pixelY) (y down; a pixel's
// centre is at + 0.5) in the triangle with clip positions c0, c1, c2
// (x, y, z, w), at render size width x height.
Barycentrics barycentricsAt(const float c0[4], const float c1[4], const float c2[4], float pixelX,
                            float pixelY, float width, float height);

} // namespace meridian
