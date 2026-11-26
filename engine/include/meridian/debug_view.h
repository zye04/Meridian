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
  // Texture stretch of the base colour texture on the surface: hue by the
  // ratio of its texel density along and across (green 1:1, yellow 2:1,
  // red 4:1 and over) over an 8 x 8 checker of the texture; grey where
  // there is no texture (terrain, voxels, untextured).
  Stretch,
  // One part of the light (exposed, before contact shadows, bloom and the
  // grade), each through the same neutral tonemap: the sun (diffuse +
  // specular, transmission included), the ambient (sky, ground, probes;
  // diffuse + specular), and all specular (sun + ambient).
  Sun,
  Ambient,
  Specular,
  // The frame's luminance in EV relative to 18% grey after exposure, one
  // colour per EV (luminanceViewLegend).
  Luminance,
  Count,
};

const char *debugViewName(DebugView view);
// The lod view's colours (kind + level -> sRGB), one per line, from the
// same formulas as debug_view.frag; the report writes it as legend.txt.
std::string lodViewLegend();
std::optional<DebugView> parseDebugView(std::string_view name);
// The luminance view's colours per EV, the same way.
std::string luminanceViewLegend();

// Solo or hide one instance or material (Renderer::setDebugFilter): the
// scene pipelines' debug set removes the rest (solo) or it (hide) from the
// frame; shadows, GI and blended surfaces keep everything.
enum class DebugFilterMode : uint32_t { None, Solo, Hide };
struct DebugFilter {
  DebugFilterMode mode = DebugFilterMode::None;
  bool material = false; // else the 24-bit instance id
  uint32_t target = 0;
};

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
  // The light split (the Sun/Ambient/Specular views' values) in cd/m^2,
  // before contact shadows: sun and ambient, each diffuse and specular,
  // haze transmittance applied (their sum + haze inscatter + emissive is
  // the HDR value). False where the pixel shades none (sky, blended).
  bool lit = false;
  float sunDiffuse[3] = {};
  float sunSpecular[3] = {};
  float ambientDiffuse[3] = {};
  float ambientSpecular[3] = {};
  // Base colour texture density (texels/m, geometric mean of the two
  // directions) and stretch (their ratio); 0 where none.
  float texelsPerMeter = 0.0f;
  float stretch = 0.0f;
  // The HDR frame at the pixel: RGB in cd/m^2 (after contact shadows and
  // clouds) and its EV relative to 18% grey after exposure (the Luminance
  // view's value); the exposure it was shaded with.
  float hdr[3] = {};
  float ev = 0.0f;
  float exposure = 0.0f;
};

} // namespace meridian
