#pragma once

#include <meridian/camera.h>
#include <meridian/math/mat4.h>
#include <meridian/mesh.h>

#include <cstdint>
#include <memory>
#include <span>

namespace meridian {

class Window;

struct MeshId {
  uint32_t index = 0;
};

// A planet's atmosphere, in kilometers and per-kilometer coefficients.
// Defaults are Earth's (Bruneton 2017 / Hillaire 2020). Change them for other
// worlds: radius, air density (Rayleigh), haze (Mie), ozone, ground color.
struct Atmosphere {
  float bottomRadius = 6360.0f;
  float topRadius = 6460.0f;
  Vec3 rayleighScattering = {0.005802f, 0.013558f, 0.033100f};
  float rayleighScaleHeight = 8.0f;
  Vec3 mieScattering = {0.003996f, 0.003996f, 0.003996f};
  Vec3 mieExtinction = {0.004440f, 0.004440f, 0.004440f};
  float mieScaleHeight = 1.2f;
  // Forward-scattering of haze around the sun, in [0, 1).
  float miePhaseG = 0.8f;
  Vec3 ozoneAbsorption = {0.000650f, 0.001881f, 0.000085f};
  Vec3 groundAlbedo = {0.3f, 0.3f, 0.3f};

  bool operator==(const Atmosphere &) const = default;
};

// Physical units, linear Rec.709 RGB. The sun direction points toward the
// sun. The atmosphere colors the sunlight and produces the sky.
struct Lighting {
  Vec3 sunDirection = normalize({0.5f, 0.55f, -0.4f});
  // Half the sun's apparent size; it softens shadows with distance.
  float sunAngularRadius = 0.00465f; // radians
  // Sunlight above the atmosphere (the solar illuminance constant).
  Vec3 sunIlluminance = {128000.0f, 128000.0f, 128000.0f}; // lux
  Atmosphere atmosphere;
};

struct DrawItem {
  MeshId mesh;
  Mat4 transform = Mat4::identity();
};

class Renderer {
public:
  explicit Renderer(Window &window);
  ~Renderer();

  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  // Copies the mesh into GPU-only memory. The data can be freed afterwards.
  MeshId uploadMesh(const MeshData &mesh);

  void draw(const Camera &camera, const Lighting &lighting,
            std::span<const DrawItem> items);

  // GPU time of a recent frame, measured with timestamp queries.
  float gpuFrameMs() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace meridian
