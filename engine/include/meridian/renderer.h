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

// Physical units, linear Rec.709 RGB. The sun direction points toward the
// sun. Defaults approximate a clear day with the sun about 35 degrees high.
struct Lighting {
  Vec3 sunDirection = normalize({0.5f, 0.55f, -0.4f});
  Vec3 sunIlluminance = {100000.0f, 95000.0f, 85000.0f}; // lux
  Vec3 skyLuminance = {4800.0f, 8000.0f, 12800.0f};      // cd/m^2
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
