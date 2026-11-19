#pragma once

#include <cmath>

namespace meridian {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  bool operator==(const Vec3 &) const = default;
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 a, float scalar) {
  return {a.x * scalar, a.y * scalar, a.z * scalar};
}

inline float dot(Vec3 a, Vec3 b) {
  return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }

inline Vec3 normalize(Vec3 a) { return (a * (1.0f / length(a))); }

inline Vec3 cross(Vec3 a, Vec3 b) {
  return {(a.y * b.z - a.z * b.y), (a.z * b.x - a.x * b.z),
          (a.x * b.y - a.y * b.x)};
}
} // namespace meridian
