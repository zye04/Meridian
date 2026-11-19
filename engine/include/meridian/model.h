#pragma once

#include <meridian/renderer.h>

#include <filesystem>
#include <vector>

namespace meridian {

// A loaded model: one draw item per mesh primitive instance, with transforms
// in model space.
struct Model {
  std::vector<DrawItem> drawItems;
};

// KHR_texture_transform: scale, then rotate, then offset.
struct UvTransform {
  float offset[2] = {0.0f, 0.0f};
  float scale[2] = {1.0f, 1.0f};
  float rotation = 0.0f;
};

void applyUvTransform(MeshData &mesh, const UvTransform &transform);

// Loads a glTF 2.0 file (.gltf or .glb) and uploads its meshes, materials and
// textures. Textures must be KTX2 (KHR_texture_basisu); PNG/JPEG sources are
// converted by the asset cooker first. Missing tangents are generated with
// MikkTSpace. A material's texture transform is baked into the texture
// coordinates, so all its textures must share one.
Model loadGltf(Renderer &renderer, const std::filesystem::path &path);

} // namespace meridian
