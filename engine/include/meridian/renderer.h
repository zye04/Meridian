#pragma once

#include <meridian/camera.h>
#include <meridian/debug_view.h>
#include <meridian/grade.h>
#include <meridian/math/mat4.h>
#include <meridian/mesh.h>
#include <meridian/terrain.h>
#include <meridian/texture.h>
#include <meridian/upscaler.h>
#include <meridian/voxel_shape.h>

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
struct GeoChannels;

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
// What the reference path tracer accumulates (renderer_reference.cpp):
// radiance, the primary hit's diffuse albedo (as the albedo capture), or a
// furnace check of the integrator.
enum class ReferenceMode { Radiance, Albedo, FurnaceWhite, FurnaceLeaf, FurnaceSun };

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
  // KHR_materials_diffuse_transmission: the share of diffuse light that
  // passes through a thin surface (leaves) instead of reflecting, and the
  // colour it takes (factor x texture), independent of the base colour.
  float diffuseTransmission = 0.0f;
  Vec3 diffuseTransmissionColor{1.0f, 1.0f, 1.0f};
  std::optional<TextureId> diffuseTransmissionColorTexture;
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

// Where each terrain layer goes (mesh.frag, terrainSurface): meadow grass
// on open ground; clover in damp flat hollows; mud in valley bottoms; worn
// soil on ridges and the slopes below rock; forest path at forest edges and
// forest floor inside (the forest layer's density); gravel in channels; scree at the
// foot of rock; rock on cliffs. Flat ground above the snow line is snow.
enum class GiMode { Off, Probes };

// Off keeps the triangles in every view, cascades included.
enum class VoxelLod { Off, On };

// What requestCapture saves: the displayed frame (PNG, or the linear HDR
// image the tonemap read for a .pfm path), or the linear diffuse albedo the
// shading used (render-sized .pfm; needs enableAlbedoCapture).
enum class CaptureSource { Display, DiffuseAlbedo };

enum class TerrainRole { Grass, Clover, Mud, Soil, ForestPath, ForestFloor, Gravel, Scree, Rock, Count };
constexpr int kMaxTerrainLayers = 16;

struct TerrainMaterial {
  // Variants per role, at least one each, kMaxTerrainLayers in all; a
  // large-scale noise picks one per region. Rock is triplanar, the rest
  // hex-tiled top-down projections.
  std::array<std::vector<TerrainLayer>, static_cast<size_t>(TerrainRole::Count)> layers;
  // The placement layer (PlacementLayer::id) whose density the forest path
  // and floor follow, so floor and trees agree; 0 = no forest floor.
  uint32_t forestLayerId = 0;
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
  // Km above sea level where the aerosol layer ends; 0 = no top (a plain
  // exponential).
  float mieLayerTop = 0.0f;
  // Km over which the layer's density falls to zero around mieLayerTop
  // (smoothstep from top - fade/2 to top + fade/2).
  float mieLayerFade = 0.2f;
  // Free-troposphere aerosol density, as a fraction of the layer's sea-level
  // density (mieScattering / mieExtinction are the layer's sea-level values).
  float mieBackground = 0.0f;
  float mieBackgroundScaleHeight = 2.0f;
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
  bool terrainDetail = false;     // procedural relief on the two finest terrain levels (A/B)
  bool microShadows = true;
  float microShadowStrength = 1.0f; // 0..1
  bool contactShadows = true;       // off skips the pass
  float contactLength = 1.0f;       // multiplier on the ray length, 0..3
  bool bloom = true;                // off skips the pass
  float cloudOutline = 0.0f;        // cloud silhouette and lobe sharpening, 0 = off, 0..3
  float cutoutMipBias = 1.0f;       // extra mip bias of the alpha cut-out test, -2..2
  bool stableTaa = true; // TAA: Catmull-Rom history, 3x3 reconstruction, no motion sharpen; off = sharper, less stable
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

// The most entries a view's cull lists held in one frame (main view or any
// sun shadow cascade), each list's largest phase, queue or bucket: equal to
// the need while nothing drops, a lower bound once something does.
struct CullNeed {
  uint32_t chunks = 0;
  uint32_t workItems = 0;
  uint32_t traversalNodes = 0;
  uint32_t drawnClusters = 0;
  uint32_t voxelClusters = 0;
  uint32_t voxelQuads = 0;
  // Main view only (terrain_select.comp; the late pass's retest list).
  uint32_t terrainNodes = 0;
  uint32_t retestClusters = 0;
  uint32_t dropped = 0;
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
  // The heightfield's geo channels (geo.h; geoMatches), for placement rules
  // that read them: call after setTerrain and before setPlacement.
  void setGeoChannels(const GeoChannels &geo);
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
  // Chunks, work items, clusters, terrain nodes, BVH traversal nodes and
  // voxel list entries the culling passes dropped because a list was full,
  // the most in any frame of the run, main view and sun shadow cascades
  // (nonzero means geometry or shadow is missing).
  uint32_t droppedCullEntryCount() const;
  CullDrops droppedCullEntries() const;
  // The cull lists' need over the run: the main view's, and the largest of
  // the sun shadow cascades'.
  CullNeed cullNeed(bool cascades) const;
  // The same for one sun shadow cascade (0 nearest).
  CullNeed cascadeCullNeed(uint32_t cascade) const;
  // Waits for the frames still in flight and reads their counters, so the
  // statistics above cover every frame drawn (call before reporting).
  void drainStatistics();
  // Starts the run's most-in-a-frame statistics (drops, need, cascade
  // renders) over; call after drainStatistics.
  void resetStatistics();
  // The camera jumps (a new view of a multi-view capture): waits for the
  // GPU and drops everything carried from earlier frames, so the next frame
  // starts as the first one after loading does: temporal histories (the
  // upscaler, clouds, motion), the GI caches, every shadow map, the depth
  // pyramid, the exposure, and the jitter and noise sequence.
  void cut();
  // Placement cells found visible in a recent frame.
  uint32_t drawnCellCount() const;
  // Far cells drawn as voxels in a recent frame.
  uint32_t drawnFarCellCount() const;
  // Trees drawn as prototype voxels in a recent frame (both phases).
  uint32_t drawnVoxelTreeCount() const;
  // Their bricks and voxels drawn (both phases).
  uint32_t drawnVoxelBrickCount() const;
  uint32_t drawnVoxelQuadCount() const;
  // Their screen area in pixels, that over the render's pixels (depth
  // complexity), and how many cover under one pixel.
  double drawnVoxelQuadArea() const;
  double drawnVoxelQuadComplexity() const;
  uint32_t drawnSubpixelVoxelQuads() const;
  // Cluster group BVH nodes the traversal visited in a recent frame.
  uint32_t visitedBvhNodeCount() const;

  // Level of detail: the largest simplification error allowed on screen, in
  // pixels (default 1; 0 draws full detail).
  void setLodErrorPixels(float pixels);

  // Raster (default): the sun's shadows come from cached cascaded shadow
  // maps of the placed and hand-placed geometry; Rt traces them in the TLAS
  // (for A/B comparisons).
  void setShadowMode(ShadowMode mode);
  // Builds the TLAS and every BLAS, which ShadowMode::Rt and the reference
  // path tracer trace (default off: the raster path traces nothing, and at a
  // full forest they hold ~1.9 GB). Call before addInstance and setPlacement.
  void setRayTracing(bool enabled);
  // Reference path tracer: from the next draw on, every frame adds one path
  // traced sample per pixel to an accumulation that replaces the upscaler's
  // output (so captures save it); exposure stops adapting and bloom is
  // skipped. Clears any earlier accumulation.
  void beginReference(ReferenceMode mode);
  uint32_t referenceSamples() const;
  // Median over 16x16-pixel tiles of the relative standard error of the
  // tile's mean luminance (waits for the GPU).
  double referenceNoise();
  // Cascade renders so far (a frame can render several).
  uint32_t sunShadowRenderCount() const;
  // Re-render every cascade every frame (measures the cascades' cost and
  // list need; normally they re-render only when the camera or sun moves).
  void setShadowRefresh(bool refresh);
  // The slowest sun shadows pass over the run, in ms.
  float sunShadowMaxMs() const;

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
  // On by default: the voxel levels of the meshes' cluster DAGs replace their
  // triangles where the LOD cut picks them; off keeps the triangles at every
  // distance (an A/B: what the voxels stand in for).
  void setVoxelLod(VoxelLod mode);
  // With voxel LOD on, the main view keeps the triangles of instances whose
  // origin is within this many meters of the camera (0, the default: voxels
  // wherever the LOD cut picks them); the cascades keep their voxel casters.
  void setVoxelNear(float meters);
  // The main view's voxel coverage rule (voxel_shape.h; AreaPixelPhase by
  // default, Hash for A/B); the cascades keep the hash.
  void setVoxelCoverage(VoxelCoverage rule);
  // Canopy sky (development A/B, off by default): the sky part of the
  // ambient that the GI probes don't cover (outside their clipmap, or all of
  // it with GI off) passes through the far field's forest opacity, straight up.
  enum class CanopySky : uint32_t { Off, Up };
  void setCanopySky(CanopySky mode);
  // Crown fill (development A/B, off by default): the sun light scattered
  // between a crown's leaves (multiple scattering), added as a diffuse fill
  // that grows with the crown's depth toward the sun through the mesh's
  // crown ellipsoid (triangles and leaf voxels alike), times the sun's
  // visibility at the crown's top centre.
  enum class CrownFill : uint32_t { Off, Crown };
  void setCrownFill(CrownFill mode);
  // Specular antialiasing for foliage (on by default, off for A/B):
  // leaves widen their specular lobe by the normal variation inside a pixel
  // (triangles: the shading normal's screen derivatives; voxels: the
  // neighbouring voxels' lobe axes a pixel spans), so sub-pixel glints don't
  // sparkle.
  enum class FoliageSpecularAa : uint32_t { Off, On };
  void setFoliageSpecularAa(FoliageSpecularAa mode);

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
  // visual checks; stalls that frame until the GPU is done. Each source
  // holds its own request, so one frame can save both.
  void requestCapture(const std::filesystem::path &path,
                      CaptureSource source = CaptureSource::Display);
  // Keeps the diffuse albedo target even when neither the upscaler nor GI
  // needs it, so the albedo can be captured. Call before the first frame:
  // later it recreates the targets, which resets temporal history.
  void enableAlbedoCapture();

  // Debug: waits for the GPU, then saves the weather map the last frame drew
  // as a PNG (R coverage, G type, B precipitation; row 0 = north) and prints
  // its window.
  void dumpWeatherMap(const std::filesystem::path &path);

  // Debug: waits for the GPU, then prints the last frame's exposure (EV100)
  // and sky irradiance on an upward-facing surface (relative to the sun's
  // illuminance), with the clouds and of the clear sky alone.
  void printAmbient();

  // Debug views and the pixel pick. The id target (scene attachment 7) and
  // the views' resources exist only while a view is on or keepDebugTargets
  // is set; they are made before the next frame and freed when neither is.
  void setDebugView(DebugView view);
  DebugView debugView() const;
  void keepDebugTargets(bool keep);
  // Solo/hide (debug_view.h); on, it needs the debug targets like a view.
  void setDebugFilter(const DebugFilter &filter);
  DebugFilter debugFilter() const;
  // Wait for the GPU, then: what render pixel (x, y) of the last frame shows
  // (needs the debug targets during that frame), or a view of that frame
  // saved as a display-sized PNG.
  std::optional<PixelPick> pick(uint32_t x, uint32_t y);
  bool saveDebugView(DebugView view, const std::filesystem::path &path);
  // Names reported by pick (loadGltf sets the glTF names).
  void setMeshName(MeshId mesh, std::string name);
  void setMaterialName(MaterialId material, std::string name);
  // Render size of the frames (the debug targets', pick's pixels).
  uint32_t renderWidth() const;
  uint32_t renderHeight() const;

  // Vulkan validation errors reported so far (Debug builds only).
  uint32_t validationErrorCount() const;

private:
  // The core's TAA backend creates its images through Impl.
  friend class TaaUpscaler;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace meridian
