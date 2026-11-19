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

// Loads a glTF 2.0 file (.gltf or .glb) and uploads its meshes, materials and
// textures. Textures must be KTX2 (KHR_texture_basisu); PNG/JPEG sources are
// converted by the asset cooker first. Missing tangents are generated with
// MikkTSpace.
Model loadGltf(Renderer &renderer, const std::filesystem::path &path);

} // namespace meridian
