#pragma once

#include <meridian/math/mat4.h>

#include <cmath>

namespace meridian {

// Y is up and yaw 0 looks down -Z. Positive yaw turns right, positive pitch
// looks up.
struct Camera {
  Vec3 position;
  float yaw = 0.0f;
  float pitch = 0.0f;
  float fovY = 1.2f;
  float nearPlane = 0.1f;

  Vec3 forward() const {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch),
            -std::cos(pitch) * std::cos(yaw)};
  }

  Vec3 right() const { return {std::cos(yaw), 0.0f, std::sin(yaw)}; }

  Mat4 view() const {
    return lookAt(position, position + forward(), {0.0f, 1.0f, 0.0f});
  }

  Mat4 projection(float aspect) const {
    return perspective(fovY, aspect, nearPlane);
  }
};

} // namespace meridian
