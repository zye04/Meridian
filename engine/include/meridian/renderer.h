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

// Directions point toward the light. Colors are linear RGB.
struct Lighting {
  Vec3 sunDirection = normalize({0.5f, 0.55f, -0.4f});
  Vec3 sunColor = {1.0f, 0.93f, 0.82f};
  Vec3 skyColor = {0.30f, 0.50f, 0.80f};
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

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace meridian
