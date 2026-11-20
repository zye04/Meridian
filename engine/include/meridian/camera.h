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
  // Auto-exposure bias in stops: positive brightens the image.
  float exposureCompensation = 0.0f;

  Vec3 forward() const {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch),
            -std::cos(pitch) * std::cos(yaw)};
  }

  Vec3 right() const { return {std::cos(yaw), 0.0f, std::sin(yaw)}; }

  // The view with the camera at the origin, for camera-relative positions.
  Mat4 viewRotation() const {
    return lookAt({0.0f, 0.0f, 0.0f}, forward(), {0.0f, 1.0f, 0.0f});
  }

  // Not lookAt(position, position + forward()): far from the origin that
  // sum rounds away much of forward()'s precision.
  Mat4 view() const { return viewRotation() * translation(position * -1.0f); }

  Mat4 projection(float aspect) const {
    return perspective(fovY, aspect, nearPlane);
  }
};

// Scripted capture path: `framesLeft` steps of velocity * dt before `end`.
// Computed from `end` rather than accumulated, so float rounding cannot
// leave the final frame off the requested camera.
inline Vec3 scriptedPosition(Vec3 end, Vec3 velocity, float dt,
                             int framesLeft) {
  return end - velocity * (dt * static_cast<float>(framesLeft));
}

} // namespace meridian
