#pragma once

#include <meridian/camera.h>
#include <meridian/math/mat4.h>
#include <meridian/mesh.h>
#include <meridian/texture.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace meridian {

class Window;

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
enum class AlphaMode {
  Opaque,
  // Cut out where alpha < alphaCutoff (foliage, fences). Casts shadows.
  Mask,
  // Alpha-blended over what's behind (glass). Casts no shadows.
  Blend,
};

// glTF metallic-roughness material. Factors multiply the textures.
// Upper bound on instances, sizing the instance table and each frame's TLAS.
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
  // Shaded with the renderer's TerrainMaterial; procedural colors until one
  // is set.
  bool terrain = false;
};

// One ground texture set, projected in world space.
struct TerrainLayer {
  TextureId baseColor;
  TextureId normal;
  TextureId arm; // ambient occlusion, roughness, metalness
  TextureId height;
  float tileSize = 2.0f; // meters per texture repeat
};

// Flat ground mixes soil and needles; steep slopes are rock.
struct TerrainMaterial {
  TerrainLayer soil;
  TerrainLayer needles;
  TerrainLayer rock;
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

// GPU time of one named frame pass.
struct PassTime {
  const char *name;
  float ms;
};

// A mesh placed in the world (what loadGltf returns per node primitive).
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

  // Uploads a texture into the bindless table; materials refer to it by id.
  TextureId uploadTexture(const TextureData &texture);

  MaterialId createMaterial(const MaterialData &material);

  // Textures for every material marked `terrain`.
  void setTerrainMaterial(const TerrainMaterial &material);

  // Copies the mesh into GPU-only memory. The data can be freed afterwards.
  MeshId uploadMesh(const MeshData &mesh, MaterialId material);

  // Places a mesh in the world. Instances persist and live on the GPU, which
  // culls them and builds the draw lists itself each frame.
  InstanceId addInstance(MeshId mesh, const Mat4 &transform);
  uint32_t instanceCount() const;

  // deltaSeconds: time since the previous frame, which paces exposure
  // adaptation. Pass a fixed step for repeatable output.
  void draw(const Camera &camera, const Lighting &lighting, float deltaSeconds);

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
  // Work items and clusters the culling passes dropped in a recent frame
  // because a list was full (nonzero means geometry is missing).
  uint32_t droppedCullEntryCount() const;

  // Level of detail: the largest simplification error allowed on screen, in
  // pixels (default 1; 0 draws full detail).
  void setLodErrorPixels(float pixels);

  // On by default; off draws everything in the view frustum (for A/B
  // comparisons).
  void setOcclusionCulling(bool enabled);

  // Saves the next drawn frame (after tonemapping) as a PNG. For automated
  // visual checks; stalls that frame until the GPU is done.
  void requestCapture(const std::filesystem::path &path);

  // Vulkan validation errors reported so far (Debug builds only).
  uint32_t validationErrorCount() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace meridian
