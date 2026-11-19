#pragma once

#include <meridian/math/vec3.h>

#include <cmath>

namespace meridian {

// Column-major to match GLSL: m[column][row].
struct Mat4 {
  float m[4][4] = {};

  static Mat4 identity() {
    Mat4 result;
    for (int i = 0; i < 4; ++i) {
      result.m[i][i] = 1.0f;
    }
    return result;
  }
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
  Mat4 result;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[k][row] * b.m[column][k];
      }
      result.m[column][row] = sum;
    }
  }
  return result;
}

inline Vec3 transformPoint(const Mat4 &a, Vec3 p) {
  return {a.m[0][0] * p.x + a.m[1][0] * p.y + a.m[2][0] * p.z + a.m[3][0],
          a.m[0][1] * p.x + a.m[1][1] * p.y + a.m[2][1] * p.z + a.m[3][1],
          a.m[0][2] * p.x + a.m[1][2] * p.y + a.m[2][2] * p.z + a.m[3][2]};
}

inline Mat4 translation(Vec3 offset) {
  Mat4 result = Mat4::identity();
  result.m[3][0] = offset.x;
  result.m[3][1] = offset.y;
  result.m[3][2] = offset.z;
  return result;
}

// Right-handed view matrix: the camera looks down its local -Z axis.
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
  Vec3 f = normalize(target - eye);
  Vec3 s = normalize(cross(f, up));
  Vec3 u = cross(s, f);

  Mat4 result = Mat4::identity();
  result.m[0][0] = s.x;
  result.m[1][0] = s.y;
  result.m[2][0] = s.z;
  result.m[0][1] = u.x;
  result.m[1][1] = u.y;
  result.m[2][1] = u.z;
  result.m[0][2] = -f.x;
  result.m[1][2] = -f.y;
  result.m[2][2] = -f.z;
  result.m[3][0] = -dot(s, eye);
  result.m[3][1] = -dot(u, eye);
  result.m[3][2] = dot(f, eye);
  return result;
}

// Vulkan perspective (clip Y points down, depth in [0, 1]) with reverse-Z and
// an infinite far plane: depth is 1 at the near plane and approaches 0 at
// infinity. Reverse-Z spreads float precision evenly over distance, which an
// open world needs to avoid z-fighting far away.
inline Mat4 perspective(float fovY, float aspect, float nearPlane) {
  float f = 1.0f / std::tan(fovY * 0.5f);

  Mat4 result;
  result.m[0][0] = f / aspect;
  result.m[1][1] = -f;
  result.m[2][3] = -1.0f;
  result.m[3][2] = nearPlane;
  return result;
}

} // namespace meridian
