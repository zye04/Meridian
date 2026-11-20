#pragma once

#include <meridian/camera.h>
#include <meridian/grade.h>
#include <meridian/math/mat4.h>
#include <meridian/mesh.h>
#include <meridian/terrain.h>
#include <meridian/texture.h>
#include <meridian/upscaler.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace meridian {

class Window;
class UpscalerProvider;
struct Prototype;
struct PlacementLayer;

struct MeshId {
  uint32_t index = 0;
};

struct MaterialId {
  uint32_t index = 0;
};

struct TextureId {
  uint32_t index = 0;
};

struct InstanceId {
  uint32_t index = 0;
};

// How a material's alpha (base color alpha) is used, as in glTF.
enum class ShadowMode { Raster, Rt };

enum class AlphaMode {
  Opaque,
  // Cut out where alpha < alphaCutoff (foliage, fences). Casts shadows.
  Mask,
  // Alpha-blended over what's behind (glass). Casts no shadows.
  Blend,
};

// glTF metallic-roughness material. Factors multiply the textures.
// Upper bound on (hand-placed) instances, sizing the instance table.
constexpr uint32_t kMaxInstances = 65536;

struct MaterialData {
  float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Vec3 emissiveFactor; // cd/m^2
  float metallicFactor = 1.0f;
  float roughnessFactor = 1.0f;
  float normalScale = 1.0f;
  float occlusionStrength = 1.0f;
  std::optional<TextureId> baseColorTexture;
  std::optional<TextureId> metallicRoughnessTexture;
  std::optional<TextureId> normalTexture;
  std::optional<TextureId> occlusionTexture;
  std::optional<TextureId> emissiveTexture;
  AlphaMode alphaMode = AlphaMode::Opaque;
  float alphaCutoff = 0.5f;
  // Render back faces too (thin geometry such as leaves).
  bool doubleSided = false;
  // KHR_materials_diffuse_transmission factor: the share of the diffuse
  // colour that passes through a thin surface (leaves).
  float diffuseTransmission = 0.0f;
};

// One ground texture set, projected in world space.
struct TerrainLayer {
  TextureId baseColor;
  TextureId normal;
  TextureId arm; // ambient occlusion, roughness, metalness
  TextureId height;
  float tileSize = 2.0f; // meters per texture repeat
  // Meters the height map's 0..1 range spans, for parallax; 0 = none.
  float heightScale = 0.0f;
  // How much of the AO map applies, as glTF's occlusion strength: lower it
  // where the base color already carries the cavities' darkness.
  float occlusionStrength = 1.0f;
  // Multiplies the base color: matches the layer's mean color to its
  // neighbours' without touching its detail or contrast.
  float albedoScale[3] = {1.0f, 1.0f, 1.0f};
};

// The ground turns to rock as the terrain normal's y falls from
// kRockNormalY[1] (48 degrees) to kRockNormalY[0] (58 degrees): rock only
// on cliffs (5% of the Napf is steeper than 48 degrees at 2 m, 1% than 58).
// Placement rules that follow rock (boulders) use the same values.
constexpr float kRockNormalY[2] = {0.530f, 0.669f};

// The forest placement layer's mask (its DensityRule's maskFrequency,
// maskThreshold, maskEdge) and the placement seed. The forest floor follows
// the same noise, so floor and trees agree. frequency 0: no forest floor.
struct ForestMask {
  float frequency = 0.0f;
  float threshold = 0.0f;
  float edge = 0.0f;
  uint32_t seed = 0;
};

// Where each terrain layer goes (mesh.frag, terrainSurface): meadow grass
// on open ground; clover in damp flat hollows; mud in valley bottoms; worn
// soil on ridges and the slopes below rock; forest path at forest edges and
// forest floor inside (the forest mask); gravel in channels; scree at the
// foot of rock; rock on cliffs. Flat ground above the snow line is snow.
enum class GiMode { Off, Probes };

enum class TerrainRole { Grass, Clover, Mud, Soil, ForestPath, ForestFloor, Gravel, Scree, Rock, Count };
constexpr int kMaxTerrainLayers = 16;

struct TerrainMaterial {
  // Variants per role, at least one each, kMaxTerrainLayers in all; a
  // large-scale noise picks one per region. Rock is triplanar, the rest
  // hex-tiled top-down projections.
  std::array<std::vector<TerrainLayer>, static_cast<size_t>(TerrainRole::Count)> layers;
  ForestMask forest;
  // Meters above sea level; snow fades in over the 40 m above it.
  float snowLine = 4000.0f;
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

// Glare of the eye/lens: `intensity` of the HDR image's energy is
// redistributed by a wide, heavy-tailed kernel (bloom.comp), so the sun
// reads as an edgeless glare. `scatter` in [0, 1] weights the wider levels.
// An intensity of 0 skips the pass.
struct Bloom {
  float intensity = 0.015f;
  float scatter = 0.7f;
};

// Live toggles for the effects whose look is subtle, for A/B in the grading
// panel. The defaults are the normal rendering.
struct Effects {
  bool parallax = true;           // terrain parallax occlusion mapping
  float parallaxDepth = 1.0f;     // multiplier on each layer's physical depth, 0..4
  bool parallaxSelfShadow = true; // the sun's shadow inside the parallax relief
  bool microShadows = true;
  float microShadowStrength = 1.0f; // 0..1
  bool contactShadows = true;       // off skips the pass
  float contactLength = 1.0f;       // multiplier on the ray length, 0..3
  bool bloom = true;                // off skips the pass
};

// GPU time of one named frame pass.
struct PassTime {
  const char *name;
  float ms;
};

// Entries the culling passes dropped because a list was full, per list.
struct CullDrops {
  uint32_t chunks = 0;
  uint32_t workItems = 0;
  uint32_t clusters = 0;
  uint32_t terrainNodes = 0;
  uint32_t traversalNodes = 0;
};

// A mesh placed in the world (what loadGltf returns per node primitive).
struct DrawItem {
  MeshId mesh;
  Mat4 transform = Mat4::identity();
};

class Weather;

class Renderer {
public:
  // `provider` supplies optional upscaler backends (DLSS) and outlives the
  // Renderer. `aa` is applied before the first swapchain, so the targets
  // are built once (same fallbacks as setAntiAliasing).
  explicit Renderer(Window &window, UpscalerProvider *provider = nullptr,
                    const AaSettings &aa = {});
  ~Renderer();

  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  // Uploads a texture into the bindless table; materials refer to it by id.
  TextureId uploadTexture(const TextureData &texture);

  MaterialId createMaterial(const MaterialData &material);

  // The ground: a heightfield drawn with continuous LOD (CDLOD), selected on
  // the GPU each frame. Replaces any previous terrain. Shaded with the
  // TerrainMaterial, or procedural colors until one is set.
  void setTerrain(const Heightfield &field);
  void setTerrainMaterial(const TerrainMaterial &material);

  // Places the layers' prototypes over the terrain (call after setTerrain)
  // on the GPU, deterministically: placement.h's evaluatePoint is the CPU
  // reference. The result replaces any earlier placement. Prototype indices
  // in the layers index `prototypes`. patternCache: a directory for the
  // layers' dither patterns (placement.h, cachedDitherPattern); empty = none.
  void setPlacement(std::span<const Prototype> prototypes,
                    std::span<const PlacementLayer> layers, uint32_t seed,
                    const std::filesystem::path &patternCache = {});
  uint32_t placedInstanceCount() const;
  // Streamed placement (PlacementLayer::streamFullDensity): its layers,
  // window (cells per side), ring size (instances) and the instances live
  // in all slots. Reads the slot records back; for captures, not per frame.
  struct StreamStats {
    uint32_t layers = 0, window = 0, ringInstances = 0, live = 0;
  };
  StreamStats streamStats() const;

  // Copies the mesh into GPU-only memory. The data can be freed afterwards.
  MeshId uploadMesh(const MeshData &mesh, MaterialId material);

  // Places a mesh in the world. Instances persist and live on the GPU, which
  // culls them and builds the draw lists itself each frame.
  InstanceId addInstance(MeshId mesh, const Mat4 &transform);
  uint32_t instanceCount() const;

  // Copies the weather the GPU reads (wind, current state, history) into the
  // next frame. Call every frame before draw; the renderer keeps last
  // frame's wind for motion vectors.
  void setWeather(const Weather &weather);

  // On by default; off skips the cloud march and composite (for A/B).
  void setCloudsEnabled(bool enabled);

  // deltaSeconds: time since the previous frame, which paces exposure
  // adaptation. Pass a fixed step for repeatable output.
  void draw(const Camera &camera, const Lighting &lighting, float deltaSeconds);

  // Starts an ImGui frame (Dear ImGui is the overlay): call it, issue the
  // ImGui widgets, then draw(), which renders them over the tonemapped image
  // (captures include them). Frames without a call pay nothing.
  void beginOverlay();

  // Milliseconds the last draw() spent blocked in vkWaitForFences and
  // vkAcquireNextImageKHR, i.e. waiting on the GPU or the display.
  float cpuWaitMs() const;

  // GPU time of a recent frame, measured with timestamp queries: from the
  // first to the last pass timestamp.
  float gpuFrameMs() const;
  // Per-pass GPU time of a recent frame, in frame order. Passes sum to
  // gpuFrameMs().
  std::span<const PassTime> gpuPassTimes() const;

  // Opaque and alpha-masked instances drawn in a recent frame (blended
  // instances are drawn from the CPU and not counted).
  uint32_t drawnInstanceCount() const;
  // Clusters and triangles of those instances drawn in a recent frame.
  uint32_t drawnClusterCount() const;
  uint32_t drawnTriangleCount() const;
  // Terrain quadtree nodes drawn in a recent frame (2048 triangles each).
  uint32_t drawnTerrainNodeCount() const;
  // Chunks, work items, clusters, terrain nodes and BVH traversal nodes the
  // culling passes dropped in a recent frame because a list was full
  // (nonzero means geometry is missing).
  uint32_t droppedCullEntryCount() const;
  CullDrops droppedCullEntries() const;
  // Placement cells found visible in a recent frame.
  uint32_t drawnCellCount() const;
  // Far cells drawn as voxels in a recent frame.
  uint32_t drawnFarCellCount() const;
  // Cluster group BVH nodes the traversal visited in a recent frame.
  uint32_t visitedBvhNodeCount() const;

  // Level of detail: the largest simplification error allowed on screen, in
  // pixels (default 1; 0 draws full detail).
  void setLodErrorPixels(float pixels);

  // Raster (default): the sun's shadows come from cached cascaded shadow
  // maps of the placed and hand-placed geometry; Rt traces them in the TLAS
  // (for A/B comparisons).
  void setShadowMode(ShadowMode mode);
  // Cascade renders so far (a frame can render several).
  uint32_t sunShadowRenderCount() const;

  // On by default; off draws everything in the view frustum (for A/B
  // comparisons).
  void setOcclusionCulling(bool enabled);

  // On by default: sun shadow rays from receivers beyond 4 km see distant
  // forests as the far field's voxels; off traces every tree in the TLAS
  // (for A/B comparisons). Needs a far field (setPlacement).
  void setVoxelFarShadows(bool enabled);

  // Global illumination (docs/superpowers/specs/2026-11-20-gi-design.md):
  // Off by default; Probes lights ambient from the terrain-following probe
  // clipmap traced against the heightfield and the far field.
  void setGiMode(GiMode mode);

  // The colour grade, baked with the display transform into a 3D LUT on the
  // next frame. Identity by default.
  void setGrade(const Grade &grade);

  void setBloom(const Bloom &bloom);
  void setEffects(const Effects &effects);

  // Recreates the render-sized images and the upscaler and resets the
  // temporal history. DLSS falls back to TAA when the provider doesn't
  // support the settings; it picks its own render size, TAA renders at
  // renderScale (0 < scale <= 1) of the display. antiAliasing() returns the
  // settings in effect.
  void setAntiAliasing(const AaSettings &settings);
  AaSettings antiAliasing() const;

  // Saves the next drawn frame (after tonemapping) as a PNG. For automated
  // visual checks; stalls that frame until the GPU is done.
  void requestCapture(const std::filesystem::path &path);

  // Debug: waits for the GPU, then saves the weather map the last frame drew
  // as a PNG (R coverage, G type, B precipitation; row 0 = north) and prints
  // its window.
  void dumpWeatherMap(const std::filesystem::path &path);

  // Debug: waits for the GPU, then prints the last frame's exposure (EV100)
  // and sky irradiance on an upward-facing surface (relative to the sun's
  // illuminance), with the clouds and of the clear sky alone.
  void printAmbient();

  // Vulkan validation errors reported so far (Debug builds only).
  uint32_t validationErrorCount() const;

private:
  // The core's TAA backend creates its images through Impl.
  friend class TaaUpscaler;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace meridian
