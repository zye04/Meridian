#include <meridian/visibility.h>

#include <algorithm>

namespace meridian {

VisibilityId packClusterTriangle(uint32_t instance, uint32_t cluster, uint32_t triangle,
                                 uint32_t terrainSunCode) {
  const uint32_t sun = terrainSunCode > 1 ? kVisibilityTerrainSunPerPixel : terrainSunCode;
  return {(instance & 0xFFFFFFu) | (triangle & 0x7Fu) << 24,
          (cluster & 0x3FFFFFFu) | sun << 26 |
              static_cast<uint32_t>(VisibilityKind::ClusterTriangle) << 30};
}

VisibilityId packVoxel(uint32_t phase, uint32_t index) {
  return {0, (index & 0x3FFFFFFu) | (phase & 1u) << 26 |
                 static_cast<uint32_t>(VisibilityKind::Voxel) << 30};
}

VisibilityKind visibilityKind(VisibilityId id) { return static_cast<VisibilityKind>(id.y >> 30); }

UnpackedVisibility unpackVisibility(VisibilityId id) {
  UnpackedVisibility u;
  u.kind = visibilityKind(id);
  if (u.kind == VisibilityKind::ClusterTriangle) {
    u.instance = id.x & 0xFFFFFFu;
    u.triangle = (id.x >> 24) & 0x7Fu;
    u.cluster = id.y & 0x3FFFFFFu;
    const uint32_t sun = (id.y >> 26) & 3u;
    u.terrainSunCode = sun == kVisibilityTerrainSunPerPixel ? 255u : sun;
  } else if (u.kind == VisibilityKind::Voxel) {
    u.index = id.y & 0x3FFFFFFu;
    u.phase = (id.y >> 26) & 1u;
  }
  return u;
}

Barycentrics barycentricsAt(const float c0[4], const float c1[4], const float c2[4], float pixelX,
                            float pixelY, float width, float height) {
  const float *c[3] = {c0, c1, c2};
  // Framebuffer positions of the vertices and 1 / w.
  float x[3], y[3], invW[3];
  for (int i = 0; i < 3; ++i) {
    invW[i] = 1.0f / c[i][3];
    x[i] = (c[i][0] * invW[i] * 0.5f + 0.5f) * width;
    y[i] = (c[i][1] * invW[i] * 0.5f + 0.5f) * height;
  }
  // Screen-linear weights l_i = (e_i(P)) / area2, with e_i the edge
  // function of the edge opposite vertex i; l_i = ax_i * px + ay_i * py + k_i.
  const float area2 = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
  const float invArea = 1.0f / area2;
  float l[3], ax[3], ay[3];
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    const int k = (i + 2) % 3;
    ax[i] = (y[j] - y[k]) * invArea;
    ay[i] = (x[k] - x[j]) * invArea;
    l[i] = ((x[j] - pixelX) * (y[k] - pixelY) - (x[k] - pixelX) * (y[j] - pixelY)) * invArea;
  }
  // Perspective correction: b_i = l_i / w_i / sum(l_j / w_j).
  float s = 0.0f, sx = 0.0f, sy = 0.0f;
  float q[3];
  for (int i = 0; i < 3; ++i) {
    q[i] = l[i] * invW[i];
    s += q[i];
    sx += ax[i] * invW[i];
    sy += ay[i] * invW[i];
  }
  const float invS = 1.0f / s;
  float b[3], dbdx[3], dbdy[3];
  for (int i = 0; i < 3; ++i) {
    b[i] = q[i] * invS;
    dbdx[i] = (ax[i] * invW[i] - b[i] * sx) * invS;
    dbdy[i] = (ay[i] * invW[i] - b[i] * sy) * invS;
  }
  Barycentrics r;
  r.b = {b[0], b[1], b[2]};
  r.dbdx = {dbdx[0], dbdx[1], dbdx[2]};
  r.dbdy = {dbdy[0], dbdy[1], dbdy[2]};
  // Vulkan: a = -1/2 sum(x_i y_(i+1) - x_(i+1) y_i), front when positive
  // under VK_FRONT_FACE_COUNTER_CLOCKWISE; -area2 / 2 is the same sum.
  r.frontFacing = area2 < 0.0f;
  r.outside = std::max(0.0f, -std::min({b[0], b[1], b[2]}));
  return r;
}

} // namespace meridian
