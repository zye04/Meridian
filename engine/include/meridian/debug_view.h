#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace meridian {

// What the screen shows instead of the tonemapped frame (Renderer::setDebugView).
// The views read the render-sized scene attachments directly, before the
// upscaler, so ids and colours don't smear.
enum class DebugView : uint32_t {
  None,
  // The diffuse albedo the shading used (scene attachment 3: base colour
  // times 1 - metallic; the voxels' mean leaf colour).
  BaseColor,
  // Triangle clusters green to red by DAG level (0 = full detail), voxel
  // clusters blue to violet by level, far cells cyan, terrain grey; cluster
  // borders by a per-cluster brightness hash.
  Lod,
  Count,
};

const char *debugViewName(DebugView view);
// The lod view's colours (kind + level -> sRGB), one per line, from the
// same formulas as debug_view.frag; the report writes it as legend.txt.
std::string lodViewLegend();
std::optional<DebugView> parseDebugView(std::string_view name);

// What a pixel shows. Must match the kinds in debug_id.glsl.
enum class PickKind : uint32_t { Sky, Triangles, Voxels, FarCell, Terrain };

const char *pickKindName(PickKind kind);

// The surface at one render pixel of the last frame (Renderer::pick).
struct PixelPick {
  uint32_t x = 0;
  uint32_t y = 0;
  PickKind kind = PickKind::Sky;
  // Instances and clusters: the 24-bit instance id (placed when >= the
  // placed base) and the global cluster index; far cells: the cell index
  // and the far level.
  uint32_t instance = 0;
  bool placed = false;
  std::optional<uint32_t> cluster;
  std::optional<uint32_t> lodLevel;
  float voxelEdge = 0.0f;
  std::optional<uint32_t> farCell;
  std::optional<uint32_t> mesh;
  std::optional<uint32_t> material;
  // glTF names (Renderer::setMeshName / setMaterialName), empty if unnamed.
  std::string meshName;
  std::string materialName;
  // Metres from the camera (flat world); infinite on the sky.
  float distance = 0.0f;
  // Linear diffuse albedo (the BaseColor view's value).
  float baseColor[3] = {0.0f, 0.0f, 0.0f};
};

} // namespace meridian
