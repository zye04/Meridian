#include <meridian/visibility.h>

#include <algorithm>
#include <cmath>

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
  // The pixel's ray meets the triangle where sum k_i (x_i, y_i, w_i) is
  // proportional to (nx, ny, 1), with (x, y, w) the vertices' clip positions
  // and (nx, ny) the pixel in NDC: k = adj(M) (nx, ny, 1), M's columns the
  // vertices, b = k / sum(k) (2D homogeneous rasterization, Olano and Greer
  // 1997). No division by w, so a vertex behind the camera (w <= 0, clipped
  // by the rasterizer) gives the same weights as the visible part.
  const float *c[3] = {c0, c1, c2};
  float x[3], y[3], w[3];
  const bool inFront = c0[3] > 0.0f && c1[3] > 0.0f && c2[3] > 0.0f;
  for (int i = 0; i < 3; ++i) {
    w[i] = c[i][3];
    x[i] = c[i][0];
    y[i] = c[i][1];
    if (inFront) {
      // The rasterizer's vertices: snapped to its 1/256 px grid.
      const float sx = std::round((x[i] / w[i] * 0.5f + 0.5f) * width * 256.0f) / 256.0f;
      const float sy = std::round((y[i] / w[i] * 0.5f + 0.5f) * height * 256.0f) / 256.0f;
      x[i] = (sx / width * 2.0f - 1.0f) * w[i];
      y[i] = (sy / height * 2.0f - 1.0f) * w[i];
    }
  }
  // k_i = det(c_j, c_k, p) with p = (nx, ny, 1) = det(a_j, a_k, p) with
  // a = c - w p: the vertices relative to the pixel's ray, whose third
  // component is 0, so k_i = a_j x a_k (2D). Small, pixel-relative values:
  // no cancellation between large clip coordinates.
  const float nx = pixelX / width * 2.0f - 1.0f;
  const float ny = pixelY / height * 2.0f - 1.0f;
  const float dnx = 2.0f / width;
  const float dny = 2.0f / height;
  float rx[3], ry[3];
  for (int i = 0; i < 3; ++i) {
    rx[i] = x[i] - w[i] * nx;
    ry[i] = y[i] - w[i] * ny;
  }
  // q = k, qx = dk / dnx, qy = dk / dny.
  float q[3], qx[3], qy[3];
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    const int k = (i + 2) % 3;
    q[i] = rx[j] * ry[k] - ry[j] * rx[k];
    qx[i] = w[k] * ry[j] - w[j] * ry[k];
    qy[i] = w[j] * rx[k] - w[k] * rx[j];
  }
  const float s = q[0] + q[1] + q[2];
  const float sx = (qx[0] + qx[1] + qx[2]) * dnx;
  const float sy = (qy[0] + qy[1] + qy[2]) * dny;
  const float invS = 1.0f / s;
  float b[3], dbdx[3], dbdy[3];
  for (int i = 0; i < 3; ++i) {
    b[i] = q[i] * invS;
    dbdx[i] = (qx[i] * dnx - b[i] * sx) * invS;
    dbdy[i] = (qy[i] * dny - b[i] * sy) * invS;
  }
  Barycentrics r;
  r.b = {b[0], b[1], b[2]};
  r.dbdx = {dbdx[0], dbdx[1], dbdx[2]};
  r.dbdy = {dbdy[0], dbdy[1], dbdy[2]};
  // det(M) = sum(w_i k_i) = the point's w x sum(k), so at a visible point
  // sum(k) has det's sign. Vulkan's facing for VK_FRONT_FACE_COUNTER_CLOCKWISE
  // (a = -1/2 sum(x_i y_(i+1) - x_(i+1) y_i) positive, y down) is a negative
  // NDC area, the sign of det(M) when all w > 0.
  r.frontFacing = s < 0.0f;
  r.outside = std::max(0.0f, -std::min({b[0], b[1], b[2]}));
  return r;
}

} // namespace meridian
