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
  // Exposure value at ISO 100. ~15 suits a sunny day ("sunny 16" rule).
  float ev100 = 15.0f;

  Vec3 forward() const {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch),
            -std::cos(pitch) * std::cos(yaw)};
  }

  Vec3 right() const { return {std::cos(yaw), 0.0f, std::sin(yaw)}; }

  Mat4 view() const {
    return lookAt(position, position + forward(), {0.0f, 1.0f, 0.0f});
  }

  // Scale from scene luminance to display range. The 1.2 factor maps the
  // luminance that saturates a real sensor at this EV to 1.0 (Lagarde & de
  // Rousiers, "Moving Frostbite to PBR", 2014).
  float exposure() const { return 1.0f / (1.2f * std::exp2(ev100)); }

  Mat4 projection(float aspect) const {
    return perspective(fovY, aspect, nearPlane);
  }
};

} // namespace meridian
