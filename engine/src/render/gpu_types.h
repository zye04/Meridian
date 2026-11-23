// GPU-side constants and data layouts. Structs marked "must match" mirror
// GLSL declarations in engine/shaders; change both together.
#pragma once

#include <meridian/farfield.h>
#include <meridian/placement.h>
#include <meridian/renderer.h>
#include <meridian/shadow_cascades.h>
#include <meridian/weather.h>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define MERIDIAN_HAS_LSAN 1
#endif
#endif

namespace meridian {

// Two frames in flight: the CPU records frame N+1 while the GPU renders N.
constexpr uint32_t kFramesInFlight = 2;
// Named GPU timestamps per frame (renderer_frame.cpp); one more query starts the frame.
constexpr uint32_t kMaxPassTimes = 32;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// 16-bit float keeps smooth gradients across the full HDR range.
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Screen-space motion vectors for temporal anti-aliasing (and later DLSS).
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kTransparencyMaskFormat = VK_FORMAT_R8_UNORM;
// The sun's exposed radiance per pixel, for the contact shadow pass.
// RGBA16F, not B10G11R11: rounding up in the packed format would remove
// more sun than the pixel got and tint the contact shadow.
constexpr VkFormat kSunLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Ray Reconstruction guides: diffuse and specular albedo, world normal +
// perceptual roughness.
constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kNormalRoughnessFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kHitDistanceFormat = VK_FORMAT_R16_SFLOAT;
// Exposure for the very first frame, before any histogram exists: a sunny day.
constexpr float kInitialEv100 = 15.0f;
// Capacities of the bindless material and texture tables.
constexpr uint32_t kMaxMaterials = 16384;
constexpr uint32_t kMaxTextures = 16384;
constexpr uint32_t kDfgLutSize = 128;
// Depth pyramid levels: enough for a 32768-pixel-wide screen.
constexpr uint32_t kMaxPyramidLevels = 16;
constexpr uint32_t kMaxBloomLevels = 8;
// Shared geometry pools (bump-allocated; real streaming will need an
// allocator with frees) grow by at least 1.5x when uploadMesh needs more,
// from kMinPoolBytes. What bounds them besides maxBufferSize: work
// items pack a first cluster index into 26 bits (cull.glsl), vertex offsets
// are int32, other offsets uint32; kMaxMeshes sizes the fixed mesh table.
constexpr VkDeviceSize kMinPoolBytes = 1u << 20;
constexpr VkBufferUsageFlags kPoolUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags kVertexPoolUsage =
    kPoolUsage | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr VkBufferUsageFlags kIndexPoolUsage = kVertexPoolUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
constexpr uint32_t kMaxClusterIndex = 1u << 26;
constexpr uint32_t kMaxMeshes = 65536;
// Cluster draw lists: opaque, opaque double-sided, masked, masked
// double-sided. Blended meshes (kDrawBucketBlended) are drawn from the CPU,
// sorted back to front.
constexpr uint32_t kDrawBuckets = 4;
// Per culling phase: work items (up to 64 clusters of one instance each) and
// drawn clusters per draw list; clusters to retest in phase 1. Overflow is
// dropped and counted, and the counts that size indirect dispatches stay at
// capacity (the device must allow dispatches that large).
constexpr uint32_t kMaxWorkItems = 1u << 21;
// Per culling phase: instance chunks (up to 64 consecutive ids) the cell cull
// emits. Every cell of the crop visible needs ~55K + 2.9M / 64 = ~100K.
constexpr uint32_t kMaxChunks = 1u << 17;
constexpr uint32_t kMaxDrawnClusters = 1u << 21;
constexpr uint32_t kMaxRetestClusters = 1u << 20;
// The cluster BVH traversal: kBvhDepths dispatches per phase at most, over
// three node queues of (instance, node) entries (cluster_traverse.comp).
constexpr uint32_t kBvhDepths = 12;
constexpr uint32_t kMaxTraversalNodes = 1u << 21;
constexpr uint32_t kDrawBucketBlended = 4;

#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkDeviceAddress address = 0;
  void *mapped = nullptr;
  VkDeviceSize size = 0;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkImageView view = VK_NULL_HANDLE;
};

struct AccelerationStructure {
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  Buffer buffer;
  VkDeviceAddress address = 0;
  VkDeviceSize size = 0;
};

// CPU-side record of an uploaded mesh; its geometry is a range of the shared
// pools.
struct GpuMesh {
  uint32_t firstIndex = 0;
  uint32_t indexCount = 0;
  int32_t vertexOffset = 0;
  uint32_t materialIndex = 0;
  uint32_t drawBucket = 0;
  AlphaMode alphaMode = AlphaMode::Opaque;
  bool doubleSided = false;
  uint32_t vertexCount = 0;
  // Bounding sphere (MeshInfo's), for placement's cell bounds.
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  // Bottom-level acceleration structure: the mesh's triangles for ray
  // tracing. Built when an instance first uses the mesh (placed prototypes
  // have their own).
  AccelerationStructure blas;
};

// Must match AtmosphereParameters in common.glsl (std430).
struct GpuAtmosphere {
  Vec3 rayleighScattering;
  float bottomRadius = 0.0f;
  Vec3 mieScattering;
  float topRadius = 0.0f;
  Vec3 mieExtinction;
  float rayleighDensityExpScale = 0.0f;
  Vec3 absorptionExtinction;
  float mieDensityExpScale = 0.0f;
  Vec3 groundAlbedo;
  float miePhaseG = 0.0f;
  float absorptionLayerWidth = 0.0f;
  float absorption0Linear = 0.0f;
  float absorption0Constant = 0.0f;
  float absorption1Linear = 0.0f;
  float absorption1Constant = 0.0f;
  // The aerosol layer's smoothstep edges (km; low > 1e8 when there is no top),
  // and the free-troposphere density (fraction of the layer's) with its
  // exp scale.
  float mieLayerLow = 1e9f;
  float mieLayerHigh = 2e9f;
  float mieBackground = 0.0f;
  float mieBackgroundExpScale = 0.0f;
  float pad[3] = {};
};
static_assert(sizeof(GpuAtmosphere) == 128);

inline GpuAtmosphere toGpu(const Atmosphere &a) {
  GpuAtmosphere g;
  g.bottomRadius = a.bottomRadius;
  g.topRadius = a.topRadius;
  g.rayleighScattering = a.rayleighScattering;
  g.rayleighDensityExpScale = -1.0f / a.rayleighScaleHeight;
  g.mieScattering = a.mieScattering;
  g.mieExtinction = a.mieExtinction;
  g.mieDensityExpScale = -1.0f / a.mieScaleHeight;
  g.miePhaseG = a.miePhaseG;
  if (a.mieLayerTop > 0.0f) {
    const float fade = std::max(a.mieLayerFade, 1e-3f);
    g.mieLayerLow = a.mieLayerTop - 0.5f * fade;
    g.mieLayerHigh = a.mieLayerTop + 0.5f * fade;
  }
  g.mieBackground = a.mieBackground;
  g.mieBackgroundExpScale = -1.0f / a.mieBackgroundScaleHeight;
  g.absorptionExtinction = a.ozoneAbsorption;
  g.groundAlbedo = a.groundAlbedo;
  // Earth's ozone: density rises linearly to a peak at 25 km, then falls
  // linearly to zero at 40 km.
  g.absorptionLayerWidth = 25.0f;
  g.absorption0Linear = 1.0f / 15.0f;
  g.absorption0Constant = -2.0f / 3.0f;
  g.absorption1Linear = -1.0f / 15.0f;
  g.absorption1Constant = 8.0f / 3.0f;
  return g;
}

// Must match TerrainLayer in common.glsl.
struct GpuTerrainLayer {
  uint32_t baseColor = 0;
  uint32_t normal = 0;
  uint32_t arm = 0;
  uint32_t height = 0;
  float tileSize = 0.0f;
  float heightScale = 0.0f;
  // The camera position wrapped by the layer's period (renderer_frame.cpp).
  float offset[3] = {};
  float occlusionStrength = 1.0f;
  float albedoScale[3] = {1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(GpuTerrainLayer) == 52);

// Terrain texture coordinates (mesh.frag) are the camera-relative position
// plus the camera position wrapped by a period, so they stay small and
// precise anywhere on the map. Layers repeat every kTerrainWrapTiles tiles;
// along the hex rows (z for the planar layers, y for the rock's hex-tiled
// sides) they repeat every kTerrainHexWrapRows rows of hex cells (sqrt(3)/4
// tiles each, so 256.3 tiles), since no whole number of tiles is a whole
// number of rows. The ground's mix noise repeats
// every kTerrainNoiseWrap meters.
// Must match kHexCellsPerRepeat, kHexWrapColumns, kHexWrapRows,
// kNoiseCell and kNoiseWrapCells in mesh.frag.
constexpr int kTerrainWrapTiles = 256;
constexpr int kTerrainHexCellsPerRepeat = 2;
constexpr int kTerrainHexWrapColumns = 512;
constexpr int kTerrainHexWrapRows = 592;
constexpr double kTerrainNoiseCell = 40.0;
constexpr int kTerrainNoiseWrapCells = 100;
constexpr double kTerrainNoiseWrap = kTerrainNoiseCell * kTerrainNoiseWrapCells;
static_assert(kTerrainHexWrapColumns == kTerrainHexCellsPerRepeat * kTerrainWrapTiles);
static_assert(kTerrainHexWrapRows % 2 == 0,
              "a period along z is the hex lattice step (-rows / 2, rows)");
static_assert(kTerrainNoiseWrapCells % 10 == 0,
              "mesh.frag's noise octaves at 0.1x to 8x need whole cells per wrap");

// CDLOD terrain quadtree (terrain.glsl): level 0 nodes are 16 m, 32x32
// quads of 0.5 m; each level doubles. The root covers 1049 km around the
// origin: the horizon up to ~20 km altitude. Level 0's LOD range in meters;
// each level's range doubles.
constexpr uint32_t kTerrainRootLevel = 16;
// Taller than any placed object or model above the terrain, for the sun
// shadow cascades' depth range.
constexpr double kTallestCaster = 150.0;
static_assert(kTerrainRootLevel <= 16,
              "terrain node encoding (terrain.glsl) holds 16-bit x/z and a 5-bit level");
constexpr float kTerrainLodRange0 = 80.0f;
// Must match terrain.glsl. Capacity of each selection list and of the
// terrain draw list. Overflow is dropped and counted in
// DrawCounters::overflow.
constexpr uint32_t kMaxTerrainNodes = 8192;
// Must match terrain.glsl. Mesh shader workgroups per drawn node (4x4
// blocks of 8x8 quads).
constexpr uint32_t kTerrainTasksPerNode = 16;
constexpr uint32_t kMaxHeightPyramidLevels = 16;

// Must match Terrain in common.glsl. Layers grouped by TerrainRole, in
// role order; rock (triplanar) last.
constexpr int kTerrainRoles = static_cast<int>(TerrainRole::Count);
struct GpuTerrain {
  GpuTerrainLayer layers[kMaxTerrainLayers];
  // Per TerrainRole: its first layer | variant count << 8.
  uint32_t roles[kTerrainRoles] = {};
  uint32_t layerCount = 0;
  // A TerrainMaterial is set (procedural colors otherwise).
  uint32_t enabled = 0;
  float snowLine = TerrainMaterial{}.snowLine;
  // Bindless texture indices: the samples (R16_UNORM, row 0 = north) and
  // the normal map (RG16_SNORM normal x, z; mipmapped, mirrored repeat).
  uint32_t heightTexture = 0;
  uint32_t normalTexture = 0;
  // Min/max pyramid levels >= 1 (min | max << 16), level k at
  // pyramidOffsets[k].
  VkDeviceAddress pyramid = 0;
  // This frame's selected nodes (terrain_select.comp -> terrain.mesh).
  VkDeviceAddress nodes = 0;
  uint32_t pyramidOffsets[kMaxHeightPyramidLevels] = {};
  // Stored levels: 1..pyramidLevels.
  uint32_t pyramidLevels = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  float spacing = 0.0f;
  float minHeight = 0.0f;
  float heightScale = 0.0f;
  float lodRange0 = 0.0f;
  uint32_t rootLevel = 0;
  // The camera's x/z wrapped by kTerrainNoiseWrap.
  float noiseOffset[2] = {};
  float rockNormalY[2] = {kRockNormalY[0], kRockNormalY[1]};
  // The trees' ramp over the forest mask noise (placement.glsl, the forest
  // rule's maskPeriod; 0 = no forest floor).
  uint32_t forestMaskPeriod = 0;
  int32_t forestMaskLo = 0;
  int32_t forestMaskHi = 0;
  uint32_t forestMaskSeed = 0;
  uint32_t padding[2] = {}; // keeps the next SceneData member 16-byte aligned
};
static_assert(sizeof(GpuTerrain) == 1040);

inline GpuTerrainLayer toGpu(const TerrainLayer &layer) {
  return {layer.baseColor.index, layer.normal.index, layer.arm.index,
          layer.height.index, layer.tileSize, layer.heightScale, {}, layer.occlusionStrength,
          {layer.albedoScale[0], layer.albedoScale[1], layer.albedoScale[2]}};
}

// Gusts (wind.glsl): the phase advances this many noise cells per game second
// and wraps at the noise's period along it (passed in GpuWind).
constexpr double kGustPhaseRate = 0.25;
constexpr double kGustPhasePeriod = 256.0;

// Must match Wind in common.glsl (see wind.glsl). The 10 m wind; offset is
// the integral of its velocity (Weather::windOffset at 10 m).
struct GpuWind {
  float direction[2] = {1.0f, 0.0f};
  float speed10m = 0.0f;
  float gustiness = 0.0f;
  float offset[2] = {};
  float gustPhase = 0.0f;
  float shearExponent = kWindShearExponent;
  float veer = kWindVeer;
  float veerAltitude = kWindVeerAltitude;
  float gustPhasePeriod = static_cast<float>(kGustPhasePeriod);
  float pad = 0.0f;
};
static_assert(sizeof(GpuWind) == 48);

// Must match WeatherHistorySample in common.glsl.
static_assert(sizeof(WeatherHistorySample) == 16);

// Must match SunShadowCascade in common.glsl. `origin` is the cascade's
// center relative to the camera; the light basis is shared (GpuSunShadows).
struct alignas(16) GpuSunShadowCascade {
  float origin[3] = {};
  float halfSize = 0.0f;
  float texel = 0.0f;
  float depthNear = 0.0f;
  float depthFar = 0.0f;
  uint32_t texture = 0;
  float radius = 0.0f;
  uint32_t valid = 0;
  uint32_t pad[2] = {};
};
static_assert(sizeof(GpuSunShadowCascade) == 48);

// Must match SunShadows in common.glsl: the light basis (u, v across the
// map, d away from the sun) and one cascade per kShadowCascades.
struct alignas(16) GpuSunShadows {
  GpuSunShadowCascade cascades[kShadowCascades];
  float u[3] = {};
  float pad0 = 0.0f;
  float v[3] = {};
  float pad1 = 0.0f;
  float d[3] = {};
  float pad2 = 0.0f;
};
static_assert(sizeof(GpuSunShadows) == 288);

// Must match Weather in common.glsl (see weather.glsl). History samples are
// oldest first, their travel relative to the current travel (<= 0) so it
// stays precise however far the air has moved.
struct GpuWeather {
  float coverage = 0.0f;
  float type = 1.0f;
  float precipitation = 0.0f;
  float frontLineUpwind = kFrontLineUpwind;
  uint32_t historyCount = 0;
  uint32_t pad = 0;
  VkDeviceAddress history = 0;
};
static_assert(sizeof(GpuWeather) == 32);

// Clouds (clouds.glsl). Layer heights above sea level (m). The weather map:
// 512^2 texels over a 60 km window around the camera, texel-snapped.
constexpr float kCumulusBottom = 2800.0f;
constexpr float kCumulusTop = 4800.0f;
constexpr float kStratusBottom = 1700.0f;
constexpr float kStratusTop = 2800.0f;
constexpr uint32_t kWeatherMapResolution = 512;
constexpr double kWeatherMapSize = 60000.0;
// The weather map and the noise volumes move with the wind at this altitude.
constexpr float kCloudWindAltitude = 3000.0f;
// World-space tile periods (m). Every cloud noise tiles with kCloudNoisePeriod,
// so the wind offset is uploaded modulo it and stays precise however far the
// air has moved.
constexpr uint32_t kCloudNoisePeriod = 1u << 18;
constexpr uint32_t kWeatherCell = 4096;
constexpr uint32_t kBaseNoisePeriod = 8192;
constexpr uint32_t kDetailNoisePeriod = 256;
constexpr uint32_t kCurlNoisePeriod = 4096;
static_assert(kCloudNoisePeriod % kWeatherCell == 0 && kCloudNoisePeriod % kBaseNoisePeriod == 0 &&
              kCloudNoisePeriod % kDetailNoisePeriod == 0 &&
              kCloudNoisePeriod % kCurlNoisePeriod == 0);
// Volume sizes in texels (cloud_noise.comp).
constexpr uint32_t kBaseNoiseSize = 128;
constexpr uint32_t kDetailNoiseSize = 64;
constexpr uint32_t kCurlNoiseSize = 128;
// The weather noise's time coordinate: one lattice unit per this many game
// seconds (cells form and dissolve), periodic every kWeatherTimePeriod units.
constexpr double kWeatherCellLifetime = 1800.0;
constexpr uint32_t kWeatherTimePeriod = 64;
// The cloud shadow map: 1024^2 texels over 30 km (30 m texels) across the
// sun, centered on the camera, texel-snapped in the sun's frame.
constexpr uint32_t kCloudShadowResolution = 1024;
constexpr double kCloudShadowSize = 30720.0;
// The sky capture (sky_capture.comp): an octahedral map of the whole sphere
// around the camera; each frame updates one texel of every 2x2 block.
constexpr uint32_t kSkyCaptureResolution = 64;

// Must match Clouds in common.glsl (see clouds.glsl).
struct GpuClouds {
  float cumulusBottom = kCumulusBottom;
  float cumulusTop = kCumulusTop;
  float stratusBottom = kStratusBottom;
  float stratusTop = kStratusTop;
  float mapOrigin[2] = {};
  float mapSize = static_cast<float>(kWeatherMapSize);
  uint32_t mapResolution = kWeatherMapResolution;
  float windOffset[2] = {};
  float time = 0.0f;
  float timePeriod = static_cast<float>(kWeatherTimePeriod);
  float noisePeriod = static_cast<float>(kCloudNoisePeriod);
  float weatherCell = static_cast<float>(kWeatherCell);
  float baseNoisePeriod = static_cast<float>(kBaseNoisePeriod);
  float detailNoisePeriod = static_cast<float>(kDetailNoisePeriod);
  float curlNoisePeriod = static_cast<float>(kCurlNoisePeriod);
  uint32_t weatherMap = 0;
  uint32_t baseNoise = 0;
  uint32_t detailNoise = 0;
  uint32_t curlNoise = 0;
  uint32_t skyCapture = 0;
  // How far the cloud air moved since last frame (xz, m), for reprojection
  // and motion vectors.
  float windDisplacement[2] = {};
  // The cloud shadow map's frame (cloudShadowBasis), its size across the sun
  // (m; 0 = no cloud shadows), and the camera's offset from the map's center
  // in (right, up) (m).
  float shadowRight[3] = {};
  float shadowSize = 0.0f;
  float shadowUp[3] = {};
  uint32_t shadowResolution = kCloudShadowResolution;
  float shadowForward[3] = {};
  uint32_t shadowMap = 0;
  float shadowCamera[2] = {};
  // Coverage at the camera: the ground bounce's sunlit fraction.
  float groundCoverage = 0.0f;
  float pad1 = 0.0f;
};
static_assert(sizeof(GpuClouds) == 160);
static_assert(offsetof(GpuClouds, windDisplacement) == 88);
static_assert(offsetof(GpuClouds, shadowRight) == 96);

// Must match SceneBuffer in the shaders (std430: each vec3 takes 16 bytes).
struct SceneData {
  Mat4 viewProjection;
  Mat4 currentViewProjection;
  Mat4 previousViewProjection;
  Vec3 sunDirection;
  float mipBias = 0.0f;
  Vec3 sunIlluminance;
  uint32_t frameIndex = 0;
  Vec3 cameraPosition;
  float sunAngularRadius = 0.0f;
  Vec3 previousCameraPosition;
  uint32_t whiteNoise = 0;
  VkDeviceAddress tlas = 0;
  VkDeviceAddress exposure = 0;
  VkDeviceAddress skyIrradiance = 0;
  float inverseResolution[2] = {};
  VkDeviceAddress vertices = 0;
  VkDeviceAddress indices = 0;
  VkDeviceAddress meshes = 0;
  VkDeviceAddress instances = 0;
  VkDeviceAddress materials = 0;
  uint32_t instanceCount = 0;
  uint32_t pad1 = 0;
  float frustumPlanes[6][4] = {};
  GpuAtmosphere atmosphere;
  GpuTerrain terrain;
  // For occlusion culling: unjittered view (this frame's and last frame's,
  // which made the pyramid phase 0 tests against) and the projection's x/y
  // scale and near plane.
  Mat4 view;
  Mat4 previousView;
  float projectionScale[2] = {};
  float nearPlane = 0.0f;
  // LOD selection: the largest allowed simplification error on screen.
  float lodErrorPixels = 1.0f;
  // Cluster hierarchies and the culled cluster draw lists.
  VkDeviceAddress clusters = 0;
  VkDeviceAddress clusterVertices = 0;
  VkDeviceAddress clusterTriangles = 0;
  VkDeviceAddress drawLists = 0;
  // Placement (renderer_placement.cpp): the placed table, the cell table
  // and the cell grid (PlacementGrid).
  VkDeviceAddress placedInstances = 0;
  VkDeviceAddress cells = 0;
  int32_t cellGrid[2] = {};
  uint32_t cellGridSize[2] = {};
  // PrototypeInfo per prototype and the mesh lists they index.
  VkDeviceAddress prototypes = 0;
  VkDeviceAddress prototypeMeshes = 0;
  // The static placed instances; the streamed ring's ids follow them.
  uint32_t placedCount = 0;
  // Cells: the largest layer draw distance (0: some layer has none) and how
  // far instances' bounds reach past their cell horizontally.
  float cellDrawDistance = 0.0f;
  float cellMargin = 0.0f;
  // Whether farField is set: cells past the far switch draw their voxels.
  uint32_t farFieldEnabled = 0;
  // Streamed placement: the ring's instances, the streamed cell records
  // (after the grid's cells in `cells`), and per streamed layer
  // (fullDensity, maxDistance), per ring instance threshold | layer << 16,
  // per record its cell coordinates.
  uint32_t ringCount = 0;
  uint32_t streamRecordCount = 0;
  float cutoutMipBias = 0.0f;
  uint32_t pad5 = 0;
  VkDeviceAddress streamLayers = 0;
  VkDeviceAddress streamThresholds = 0;
  VkDeviceAddress streamCoords = 0;
  // Every mesh's cluster group BVH nodes and DAG levels.
  VkDeviceAddress clusterNodes = 0;
  VkDeviceAddress clusterLevels = 0;
  float parallaxDepthScale = 1.0f;
  float parallaxSelfShadow = 1.0f;
  float microShadowStrength = 1.0f;
  float contactLengthScale = 1.0f;
  // The global wind, this frame's and last frame's (for motion vectors).
  GpuWind wind;
  GpuWind previousWind;
  GpuWeather weather;
  // std430 aligns Clouds (vec3 members) to 16 bytes.
  alignas(16) GpuClouds clouds;
  // The far field (GpuFarField) and the list of far cells the cell cull
  // appends for voxel.mesh (cell | level << 30, one per cell).
  VkDeviceAddress farField = 0;
  VkDeviceAddress farCellList = 0;
  // Whether receivers beyond kFarShadowDistance (lighting.glsl) trace the
  // far field instead of the TLAS's far-field trees (needs farField).
  uint32_t voxelFarShadows = 0;
  // GI (gi.glsl): 0 off, 1 probes; the probe buffer (GpuGiHeader + probes).
  uint32_t giMode = 0;
  VkDeviceAddress gi = 0;
  // The far shadow map receivers read (farfield.h): its texels (two 16-bit
  // depths per word), the frame (u, v, d axes, minima, texel and depth
  // meters, size) and whether it serves this frame.
  VkDeviceAddress farShadowTexels = 0;
  float farShadowU[3] = {};
  float farShadowV[3] = {};
  float farShadowD[3] = {};
  float farShadowMinU = 0.0f, farShadowMinV = 0.0f, farShadowMinD = 0.0f;
  float farShadowTexel = 0.0f, farShadowDepth = 0.0f;
  uint32_t farShadowWidth = 0, farShadowHeight = 0;
  uint32_t farShadowMode = 0;
  uint32_t pad7 = 0;
  // Raster sun shadows: 1 in a cascade's own copy (orthographic view: no
  // curvature, no occlusion culling), the copy's pixels per meter (0 =
  // perspective), the sun shadow mode (0 raster, 1 rt) and the cascades.
  uint32_t shadowView = 0;
  float orthoPixelsPerMeter = 0.0f;
  uint32_t shadowMode = 0;
  // A shadow view's draw distances grow by the camera's largest drift
  // before the cascade re-centres, so its casters cover what the main view
  // draws until then.
  float shadowDrawMargin = 0.0f;
  GpuSunShadows sunShadows;
  // The prototype voxels (GpuProtoVoxels; 0 when none), the voxel instance
  // lists (per phase kMaxVoxelInstances entries (instance, mip), then per
  // phase kMaxVoxelBricks entries (tree entry, pool brick | terrain sun
  // << 31), then per phase kMaxVoxelQuads uints (brick entry << 6 | voxel),
  // then per phase kMaxVoxelBandQuads of the trees in the switch band)
  // and the
  // DrawCounters (voxel_instance.mesh reads its counts). voxelTrees: trees
  // past the mid switch draw as voxels.
  VkDeviceAddress protoVoxels = 0;
  VkDeviceAddress voxelInstances = 0;
  VkDeviceAddress drawCounters = 0;
  uint32_t voxelTrees = 0;
  // The terrain shadow-height map (renderer_terrain_shadow.cpp): whether it
  // serves, its texels (RG32F: the height the terrain lets the sun through
  // above, the distance to that occluder), the window's origin (x, z), unit
  // axes away from the sun and across it, texel meters, size (square) and
  // the penumbra width per meter of occluder distance.
  uint32_t terrainShadowMode = 0;
  VkDeviceAddress terrainShadowTexels = 0;
  float terrainShadowOrigin[2] = {};
  float terrainShadowAlong[2] = {};
  float terrainShadowAcross[2] = {};
  float terrainShadowTexel = 0.0f;
  uint32_t terrainShadowSize = 0;
  float terrainShadowPenumbra = 0.0f;
  // 1 in the coarsest cascade's copy: its trees cast as their far cells'
  // voxels (cell_cull.comp, voxel_shadow.frag).
  uint32_t shadowFarCells = 0;
};
// The struct members at their std430 offsets in common.glsl (spirv-dis).
static_assert(offsetof(SceneData, atmosphere) == 432);
static_assert(offsetof(SceneData, terrain) == 560);
static_assert(offsetof(SceneData, wind) == 1912);
static_assert(offsetof(SceneData, previousWind) == 1960);
static_assert(offsetof(SceneData, weather) == 2008);
static_assert(offsetof(SceneData, clouds) == 2048);
static_assert(offsetof(SceneData, farField) == 2208);
static_assert(offsetof(SceneData, gi) == 2232);
static_assert(offsetof(SceneData, farShadowTexels) == 2240);
static_assert(offsetof(SceneData, farShadowMode) == 2312);
static_assert(offsetof(SceneData, shadowView) == 2320);
static_assert(offsetof(SceneData, sunShadows) == 2336);
static_assert(offsetof(SceneData, protoVoxels) == 2624);
static_assert(offsetof(SceneData, terrainShadowTexels) == 2656);
static_assert(sizeof(SceneData) == 2704);

// Must match MeshInfo in common.glsl.
struct MeshInfo {
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  uint32_t firstIndex = 0;
  uint32_t indexCount = 0;
  int32_t vertexOffset = 0;
  uint32_t materialIndex = 0;
  uint32_t drawBucket = 0;
  uint32_t firstCluster = 0;
  uint32_t clusterCount = 0;
  uint32_t firstLevel = 0;
  uint32_t levelCount = 0;
  Vec3 halfExtent;
};
static_assert(sizeof(MeshInfo) == 64);

// Must match ClusterInfo in common.glsl. See meridian::Cluster.
struct ClusterInfo {
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  Vec3 selfCenter;
  float selfRadius = 0.0f;
  Vec3 refinedCenter;
  float refinedRadius = 0.0f;
  float selfError = 0.0f;
  float refinedError = 0.0f;
  // Into the cluster vertex pool (mesh-local vertex indices) and the
  // triangle pool (three 8-bit cluster-local indices per uint).
  uint32_t firstVertex = 0;
  uint32_t firstTriangle = 0;
  uint32_t vertexCount = 0;
  uint32_t triangleCount = 0;
  // The mesh it belongs to: placed instances name only a prototype.
  uint32_t mesh = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(ClusterInfo) == 80);

// Must match ClusterNodeInfo in common.glsl. See meridian::ClusterBvhNode;
// firstChild indexes the node pool (inner nodes) or the cluster pool
// (leaves).
struct ClusterNodeInfo {
  Vec3 lodCenter;
  float lodRadius = 0.0f;
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  float lodError = 0.0f;
  uint32_t firstChild = 0;
  uint32_t childCount = 0;
  uint32_t leaf = 0;
};
static_assert(sizeof(ClusterNodeInfo) == 48);

// Must match ClusterLevelGpu in common.glsl. See meridian::ClusterLevelInfo;
// rootNode indexes the node pool.
struct ClusterLevelGpu {
  uint32_t rootNode = 0;
  float minError = 0.0f;
  float maxError = 0.0f;
  float minGroupRadius = 0.0f;
};
static_assert(sizeof(ClusterLevelGpu) == 16);

// Must match PrototypeInfo in common.glsl. A placed prototype: its meshes
// (prototypeMeshes[firstMesh..]; a ray hit's geometry index counts from
// firstMesh), their combined bounding sphere and the BLAS holding them.
// tlasMask: its TLAS records' instance mask (kShadowCasterMask, or
// kFarFieldCasterMask in a far-field layer). voxelSize: its finest voxel (m;
// 0: it has no voxels, protovoxel.h). maxDrawDistance: the largest of its layers' (0: none).
struct PrototypeInfo {
  uint32_t firstMesh = 0;
  uint32_t meshCount = 0;
  uint32_t tlasMask = 0;
  // Prototype::tint, RGB unorm8 of tint / 2 (unpackPrototypeTint).
  uint32_t tint = 0;
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  VkDeviceAddress blasAddress = 0;
  float voxelSize = 0.0f;
  float maxDrawDistance = 0.0f;
};
static_assert(sizeof(PrototypeInfo) == 48);
static_assert(offsetof(PrototypeInfo, boundsCenter) == 16);
static_assert(offsetof(PrototypeInfo, blasAddress) == 32);

// Must match DrawCounters in cull.glsl. Indirect arguments and statistics
// written by the culling passes, reset every frame.
struct DrawCounters {
  // Per phase: vkCmdDispatchIndirect arguments for the cluster cull, one
  // workgroup per work item.
  uint32_t workDispatch[2][3];
  // Per phase and draw list: vkCmdDrawMeshTasksIndirectEXT arguments, one
  // mesh workgroup per cluster.
  uint32_t meshTasks[2][kDrawBuckets][3];
  // Phase 1's retest of the clusters phase 0 found occluded: dispatch
  // arguments (one workgroup per 64 entries) and the entry count.
  uint32_t retestDispatch[3];
  uint32_t instances;
  uint32_t triangles;
  uint32_t overflow;
  uint32_t retestCount;
  // Nodes terrain_select.comp put in the terrain draw list.
  uint32_t terrainNodes;
  // Placement cells visible in phase 1.
  uint32_t cells;
  // Per phase: the instance cull's dispatch arguments, one workgroup per
  // chunk.
  uint32_t chunkDispatch[2][3];
  // What overflow counts, per list: chunks, work items, cluster draw list
  // entries, terrain nodes, traversal nodes.
  uint32_t drops[5];
  // BVH nodes the traversal visited, both phases.
  uint32_t bvhNodes;
  uint32_t pad;
  // Per phase and BVH depth: the traversal's dispatch arguments (one
  // workgroup per 64 nodes) and node count.
  uint32_t traverseDispatch[2][kBvhDepths][3];
  uint32_t traverseCount[2][kBvhDepths];
  // vkCmdDrawMeshTasksIndirectEXT arguments for the far cells, one voxel.mesh
  // workgroup per cell (x counts the far cells drawn).
  uint32_t farTasks[3];
  uint32_t pad2;
  // Per phase: voxel_bricks.comp's dispatch (x = voxel trees, one
  // workgroup each); per phase and list (0: trees past the switch band, 1:
  // in it, drawn with the dither discard) voxel_instance.mesh's task
  // arguments (one workgroup per 64 voxels); the bricks listed per phase,
  // the voxels per phase and list; then the entries dropped from full lists.
  uint32_t voxelDispatch[2][3];
  uint32_t voxelTasks[2][2][3];
  uint32_t voxelBricks[2];
  uint32_t voxelQuads[2][2];
  uint32_t voxelDrops;
};
static_assert(sizeof(DrawCounters) == (56 + 8 * kBvhDepths + 25) * 4);
// The shaders index DrawCounters as a uint array (cull.glsl,
// terrain_select.comp).
static_assert(offsetof(DrawCounters, meshTasks) == 6 * 4);
static_assert(offsetof(DrawCounters, retestDispatch) == 30 * 4);
static_assert(offsetof(DrawCounters, instances) == 33 * 4);
static_assert(offsetof(DrawCounters, triangles) == 34 * 4);
static_assert(offsetof(DrawCounters, overflow) == 35 * 4);
static_assert(offsetof(DrawCounters, retestCount) == 36 * 4);
static_assert(offsetof(DrawCounters, terrainNodes) == 37 * 4);
static_assert(offsetof(DrawCounters, cells) == 38 * 4);
static_assert(offsetof(DrawCounters, chunkDispatch) == 39 * 4);
static_assert(offsetof(DrawCounters, drops) == 45 * 4);
static_assert(offsetof(DrawCounters, bvhNodes) == 50 * 4);
static_assert(offsetof(DrawCounters, traverseDispatch) == 52 * 4);
static_assert(offsetof(DrawCounters, traverseCount) == (52 + 6 * kBvhDepths) * 4);
static_assert(offsetof(DrawCounters, farTasks) == (52 + 8 * kBvhDepths) * 4);
// protovoxel.glsl's kCounterVoxel*.
static_assert(offsetof(DrawCounters, voxelDispatch) == 152 * 4);
static_assert(offsetof(DrawCounters, voxelTasks) == 158 * 4);
static_assert(offsetof(DrawCounters, voxelBricks) == 170 * 4);
static_assert(offsetof(DrawCounters, voxelQuads) == 172 * 4);
static_assert(offsetof(DrawCounters, voxelDrops) == 176 * 4);

// Must match InstanceData in common.glsl.
struct InstanceData {
  Mat4 transform;
  uint32_t meshIndex = 0;
  uint32_t pad[3] = {};
};
static_assert(sizeof(InstanceData) == 80);

// One instance id space (see loadInstance in common.glsl): ids below
// kPlacedBase index InstanceData, the rest are placed instances. Every id is
// also a TLAS instance custom index, which has 24 bits.
constexpr uint32_t kMaxInstanceId = (1u << 24) - 1;
// Must match kPlacedBase in common.glsl.
constexpr uint32_t kPlacedBase = kMaxInstances;
static_assert(kPlacedBase - 1 <= kMaxInstanceId);
static_assert([] {
  VkAccelerationStructureInstanceKHR instance{};
  instance.instanceCustomIndex = kMaxInstanceId;
  return instance.instanceCustomIndex == kMaxInstanceId;
}());

static_assert(sizeof(PlacedInstance) == 24); // must match common.glsl

// Must match Cell in common.glsl. Empty cells have minY = maxY = 0.
struct Cell {
  float minY, maxY;
  uint32_t firstInstance, instanceCount;
};
static_assert(sizeof(Cell) == 16);

// Must match PlacementLayer in placement.comp. Points index the uploaded
// dither patterns, prototypes the (cumulative threshold, prototype) pairs.
struct GpuPlacementLayer {
  PlacementRule rule;
  uint32_t id = 0;
  uint32_t firstPoint = 0;
  uint32_t pointCount = 0;
  uint32_t firstPrototype = 0;
  float scaleMin = 0.0f;
  float scaleMax = 0.0f;
  float sinkDepth = 0.0f;
  uint32_t fullRotation = 0;
};
static_assert(sizeof(GpuPlacementLayer) == 84);

// Must match the push constants in placement.comp.
struct PlacementConstants {
  VkDeviceAddress heights = 0; // the samples, two per uint
  VkDeviceAddress layers = 0;
  VkDeviceAddress points = 0; // uvec2 (point, threshold)
  VkDeviceAddress prototypes = 0; // uvec2 (cumulative threshold, prototype)
  VkDeviceAddress bounds = 0; // vec4 per prototype: sphere center, radius
  VkDeviceAddress counts = 0; // per (cell, layer); offsets after the scan
  VkDeviceAddress instances = 0;
  VkDeviceAddress cells = 0;
  PlacementField field;
  int32_t gridFirst[2] = {};
  uint32_t gridColumns = 0;
  uint32_t layerCount = 0;
  uint32_t seed = 0;
  float minHeight = 0.0f;
  float heightScale = 0.0f;
  float spacing = 0.0f;
};
static_assert(sizeof(PlacementConstants) == 120);

// One slot's new cell for placement.comp's PLACEMENT_STREAM variant.
struct StreamUpdate {
  uint32_t ringStart; // the slot's first ring index
  uint32_t record;    // the slot's first streamed record
  int32_t cellX = 0;
  int32_t cellZ = 0;
};
static_assert(sizeof(StreamUpdate) == 16);

// Must match the push constants in placement.comp (PLACEMENT_STREAM).
struct StreamConstants {
  VkDeviceAddress heights = 0;
  VkDeviceAddress layers = 0;
  VkDeviceAddress points = 0;
  VkDeviceAddress prototypes = 0;
  VkDeviceAddress bounds = 0;
  VkDeviceAddress updates = 0;
  VkDeviceAddress thresholds = 0;
  VkDeviceAddress coords = 0;
  VkDeviceAddress instances = 0;
  VkDeviceAddress cells = 0;
  PlacementField field;
  uint32_t seed = 0;
  uint32_t staticCount = 0; // the first ring id's index in the placed table
  uint32_t gridCells = 0;   // the first streamed record's index in the cells
  float minHeight = 0.0f;
  float heightScale = 0.0f;
  float spacing = 0.0f;
  uint32_t pad = 0;
};
static_assert(sizeof(StreamConstants) == 128);

// Must match the push constants in tlas_fill.comp.
struct TlasFillConstants {
  VkDeviceAddress placed = 0;
  VkDeviceAddress prototypes = 0;
  VkDeviceAddress records = 0; // VkAccelerationStructureInstanceKHR
  uint32_t count = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(TlasFillConstants) == 32);
static_assert(sizeof(VkAccelerationStructureInstanceKHR) == 64);

// Far field (renderer_farfield.cpp). Must match FarCell in farfield.glsl.
// Per placement cell: the canopy's extent over all levels (bottom > top
// when empty), how many of the cell's instances (the start of its run) are
// in far-field layers, and the tallest of them (m above its origin).
struct FarCell {
  float bottom = 0.0f;
  float top = 0.0f;
  uint32_t treeCount = 0;
  float treeHeight = 0.0f;
};
static_assert(sizeof(FarCell) == 16);

// Must match FarField in farfield.glsl: per level the brick columns and
// voxel pools, then FarCell per cell, the canopy top pyramid (levels
// 1..pyramidLevels, max over 2^k x 2^k cells; level 0 is FarCell::top),
// the albedo palette (vec4, linear) and the grid (FarFieldCpu's).
struct GpuFarField {
  VkDeviceAddress columns[kFarLevels] = {};
  VkDeviceAddress voxels[kFarLevels] = {};
  VkDeviceAddress cells = 0;
  VkDeviceAddress pyramid = 0;
  VkDeviceAddress palette = 0;
  float originX = 0.0f;
  float originZ = 0.0f;
  float heightBase = 0.0f;
  uint32_t cellsX = 0;
  uint32_t cellsZ = 0;
  uint32_t pyramidLevels = 0;
  uint32_t paletteSize = 0;
  uint32_t pad = 0;
  uint32_t pyramidOffsets[16] = {};
};
static_assert(sizeof(GpuFarField) == 168);

// Must match FarTemplateInfo in farfield.glsl; one per (prototype, level).
struct FarTemplateInfo {
  Vec3 centerMin;
  uint32_t firstVoxel = 0;
  Vec3 centerMax;
  uint32_t voxelCount = 0;
};
static_assert(sizeof(FarTemplateInfo) == 32);

// Must match the push constants in farfield_splat.comp.
struct FarSplatConstants {
  VkDeviceAddress placed = 0;
  VkDeviceAddress templates = 0;
  VkDeviceAddress templateVoxels = 0;
  VkDeviceAddress columns = 0;
  VkDeviceAddress accumulators = 0;
  VkDeviceAddress voxels = 0;
  VkDeviceAddress palette = 0;
  VkDeviceAddress cellData = 0;
  VkDeviceAddress headers = 0;
  float originX = 0.0f;
  float originZ = 0.0f;
  float heightBase = 0.0f;
  float fixedScale = 0.0f;
  uint32_t cellsX = 0;
  uint32_t cellsZ = 0;
  uint32_t level = 0;
  uint32_t count = 0;
  uint32_t paletteSize = 0;
  uint32_t firstBrick = 0;
  uint32_t rowBegin = 0;
  uint32_t rowEnd = 0;
};
static_assert(sizeof(FarSplatConstants) == 120);

// Must match the push constants in farfield_shadow.comp: builds texels
// [2 firstWord, 2 (firstWord + wordCount)) of the map, two per word.
struct FarShadowConstants {
  VkDeviceAddress field = 0; // GpuFarField
  VkDeviceAddress texels = 0;
  float origin[3] = {}; // the map's (minU, minV, minD) corner in world space
  float u[3] = {};
  float v[3] = {};
  float d[3] = {};
  float texel = 0.0f;
  float depth = 0.0f;
  uint32_t width = 0;
  uint32_t firstWord = 0;
  uint32_t wordCount = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(FarShadowConstants) == 88);

// Must match the push constants in terrain_shadow.comp: one workgroup per
// line of the map.
struct TerrainShadowConstants {
  VkDeviceAddress scene = 0;
  VkDeviceAddress texels = 0;
  float origin[2] = {};
  float along[2] = {};
  float across[2] = {};
  float texel = 0.0f;
  uint32_t size = 0;
  float tanE = 0.0f;
  float maxLength = 0.0f;
};
static_assert(sizeof(TerrainShadowConstants) == 56);

// Must match the push constants in farfield_check.comp (Debug).
struct FarCheckConstants {
  VkDeviceAddress field = 0; // GpuFarField
  VkDeviceAddress rays = 0;
  VkDeviceAddress results = 0;
  uint32_t count = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(FarCheckConstants) == 32);

// The prototype bake: rays per axis and voxel face, and mesh slots per
// prototype (its meshes; more share the last slot's hit counts).
constexpr uint32_t kFarBakeRays = 8;
constexpr uint32_t kFarBakeSlots = 8;
static_assert(3 * kFarBakeRays * kFarBakeRays < 256, "slot hits are 8-bit");

// Must match the push constants in farfield_bake.comp.
struct FarBakeConstants {
  VkDeviceAddress tlas = 0;
  VkDeviceAddress vertices = 0;
  VkDeviceAddress indices = 0;
  VkDeviceAddress meshes = 0;
  VkDeviceAddress materials = 0;
  VkDeviceAddress prototypeMeshes = 0;
  VkDeviceAddress bake = 0;
  uint32_t firstMesh = 0;
  uint32_t meshCount = 0;
  float origin[3] = {};
  uint32_t raysPerAxis = 0;
  uint32_t size[3] = {};
  // The first voxel of the dispatch (the prototype bake runs in chunks) and
  // the voxel edge (1 for the template).
  uint32_t firstVoxel = 0;
  float voxelSize = 1.0f;
  uint32_t pad[3] = {};
};
static_assert(sizeof(FarBakeConstants) == 112);
static_assert(offsetof(FarBakeConstants, origin) == 64);
static_assert(offsetof(FarBakeConstants, size) == 80);
static_assert(offsetof(FarBakeConstants, voxelSize) == 96);

// Must match BakeVoxel in farfield_bake.comp.
struct FarBakeVoxel {
  uint32_t passes;
  uint32_t hits;
  uint32_t slotHits[2];
  float normal[4];
  float slotColor[kFarBakeSlots][4];
  float slotTransmissionColor[kFarBakeSlots][4];
};
static_assert(sizeof(FarBakeVoxel) == 288);

// The prototype voxels (protovoxel.h), concatenated over the prototypes.
// Must match protovoxel.glsl.
constexpr uint32_t kProtoBakeRays = 4;
constexpr uint32_t kMaxVoxelInstances = 1u << 18;
constexpr uint32_t kMaxVoxelBricks = 1u << 22;
constexpr uint32_t kMaxVoxelQuads = 1u << 23;
constexpr uint32_t kMaxVoxelBandQuads = 1u << 21;
struct GpuProtoVoxelLevel {
  float voxelSize;
  // Occupied bricks: the pool's brickOffset .. + brickCount.
  uint32_t brickOffset;
  uint32_t brickCount;
  uint32_t pad;
};
static_assert(sizeof(GpuProtoVoxelLevel) == 16);
struct GpuPrototypeVoxels {
  float lo[3];
  uint32_t levelCount;
  float hi[3];
  uint32_t firstLevel;
};
static_assert(sizeof(GpuPrototypeVoxels) == 32);
struct GpuProtoVoxels {
  VkDeviceAddress prototypes = 0;
  VkDeviceAddress levels = 0;
  VkDeviceAddress voxels = 0;
  // One word per voxel, the occlusion in the low byte (read at a hit only).
  VkDeviceAddress occlusion = 0;
  // One word per pool brick: its brick coordinates in its level, x | y << 10
  // | z << 20.
  VkDeviceAddress brickCoords = 0;
};
static_assert(sizeof(GpuProtoVoxels) == 40);

// Must match the push constants in scan.comp.
struct ScanConstants {
  VkDeviceAddress values = 0;
  uint32_t count = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(ScanConstants) == 16);

// Must match ContactShadowConstants in contact_shadows.comp.
struct ContactShadowConstants {
  VkDeviceAddress sceneBuffer = 0;
  int32_t size[2] = {};
};
static_assert(sizeof(ContactShadowConstants) == 16);

// Must match ReferenceConstants in reference.comp.
struct ReferenceConstants {
  VkDeviceAddress sceneBuffer = 0;
  VkDeviceAddress sum = 0;
  VkDeviceAddress squares = 0;
  uint32_t frame = 0;
  uint32_t mode = 0;
  float cameraRight[3] = {};
  float tanHalfFovY = 0.0f;
  float cameraUp[3] = {};
  float aspect = 0.0f;
  float cameraForward[3] = {};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pad = 0;
};
static_assert(offsetof(ReferenceConstants, cameraRight) == 32);
static_assert(offsetof(ReferenceConstants, cameraUp) == 48);
static_assert(offsetof(ReferenceConstants, cameraForward) == 64);
static_assert(offsetof(ReferenceConstants, height) == 80);

// Must match depth_pyramid.comp.
struct PyramidConstants {
  int32_t sourceSize[2] = {};
  int32_t destinationSize[2] = {};
  uint32_t level = 0;
  uint32_t pad[3] = {};
};
static_assert(sizeof(PyramidConstants) == 32);

// Must match CullConstants in cull.glsl; shared by cell_cull.comp, cull.comp,
// cluster_traverse.comp and cluster_cull.comp.
struct CullConstants {
  VkDeviceAddress sceneBuffer = 0;
  // This phase's work items.
  VkDeviceAddress workItems = 0;
  VkDeviceAddress counters = 0;
  VkDeviceAddress visibility = 0;
  // 0, 1, or kCullRetest: phase 1 over the clusters phase 0 found occluded.
  uint32_t phase = 0;
  uint32_t occlusion = 0;
  uint32_t pyramidLevels = 0;
  // Whether the pyramid holds last frame's depth (phase 0 tests clusters
  // against it).
  uint32_t previousPyramid = 0;
  float pyramidSize[2] = {};
  VkDeviceAddress retest = 0;
  // This phase's instance chunks.
  VkDeviceAddress chunks = 0;
  // The traversal's three node queues and the BVH depth it processes.
  VkDeviceAddress nodeQueues = 0;
  uint32_t depth = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(CullConstants) == 88);
constexpr uint32_t kCullRetest = 2;

// Must match TerrainCounters in terrain_select.comp. Per quadtree level: the
// dispatch arguments over that level's input list (one thread per node) and
// its node count; then the terrain draw's mesh task arguments.
struct TerrainCounters {
  uint32_t dispatch[kTerrainRootLevel + 1][3];
  uint32_t count[kTerrainRootLevel + 1];
  uint32_t drawTasks[3];
  uint32_t pad;
};
static_assert(sizeof(TerrainCounters) == 288);
// terrain_select.comp indexes TerrainCounters as a uint array.
static_assert(offsetof(TerrainCounters, count) == 51 * 4);
static_assert(offsetof(TerrainCounters, drawTasks) == 68 * 4);

// Must match the push constants in terrain_select.comp.
struct TerrainSelectConstants {
  VkDeviceAddress sceneBuffer = 0;
  VkDeviceAddress counters = 0;
  // Two ping-pong selection lists of kMaxTerrainNodes entries.
  VkDeviceAddress lists = 0;
  VkDeviceAddress drawCounters = 0;
  uint32_t level = 0;
  uint32_t pad[3] = {};
};
static_assert(sizeof(TerrainSelectConstants) == 48);

// TLAS instance masks: shadow rays only test shadow casters. The placed
// trees of far-field layers cast shadows only on receivers nearer than
// kFarShadowDistance (lighting.glsl); the far field stands in beyond.
constexpr uint8_t kShadowCasterMask = 0x01;
constexpr uint8_t kNonCasterMask = 0x02;
constexpr uint8_t kFarFieldCasterMask = 0x04;

// Must match SkyIrradianceBuffer in exposure.glsl (std430: each vec3 takes 16
// bytes).
struct SkyIrradiance {
  float irradiance[3];
  float openIlluminance = 0.0f;
  float clearIrradiance[3];
  float pad1 = 0.0f;
};
static_assert(sizeof(SkyIrradiance) == 32);

// Must match ExposureBuffer in exposure.glsl.
struct ExposureState {
  float exposure = 0.0f;
  float ev100 = 0.0f;
  float previousExposure = 0.0f;
};
static_assert(sizeof(ExposureState) == 12);

constexpr uint32_t kHistogramBins = 256;

// Must match the push_constant block in post.glsl.
// Must match BloomConstants in bloom.comp.
struct BloomConstants {
  float scatter = 0.0f;
  float intensity = 0.0f;
};
static_assert(sizeof(BloomConstants) == 8);

struct PostConstants {
  float deltaSeconds = 0.0f;
  VkDeviceAddress histogram = 0;
  VkDeviceAddress exposure = 0;
  float exposureCompensation = 0.0f;
  VkDeviceAddress skyIrradiance = 0;
};
static_assert(sizeof(PostConstants) == 40);

// Must match GradeBuffer in grade_lut.comp: Grade with the white balance as
// a matrix and the exposure as a scale, both computed on the CPU.
struct GpuGrade {
  float whiteBalance[3][4] = {}; // rows; w unused
  float slope[4] = {};
  float offset[4] = {};
  float power[4] = {};
  float exposureScale = 1.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float pad = 0.0f;
  // Greens, blues, browns: centre and half-width (degrees), hue shift
  // (degrees), saturation.
  float bands[3][4] = {};
};
static_assert(sizeof(GpuGrade) == 160);

// Must match the push_constant block in grade_lut.comp.
struct GradeConstants {
  VkDeviceAddress grade = 0;
};
static_assert(sizeof(GradeConstants) == 8);

// Must match GiBuffer's header in gi.glsl: per probe level the window's
// first column, spacing and slice base; per ground cache level the window's
// first texel and the texel size; the frame; 1 to retrace every probe and
// relight every ground texel without history; the ground buffer. The probes
// (64 B each) follow.
struct GpuGiLevel {
  int32_t originX = 0, originZ = 0;
  float spacing = 0.0f, sliceBase = 0.0f;
};
struct GpuGiHeader {
  GpuGiLevel levels[5];
  GpuGiLevel ground[4];
  uint32_t frame = 0;
  uint32_t reset = 0;
  VkDeviceAddress groundBuffer = 0;
  // The probes' SH packed as half floats, the layout the shading reads
  // (gi.glsl GiVolumeBuffer).
  VkDeviceAddress volumeBuffer = 0;
  uint32_t pad[2] = {};
};
static_assert(sizeof(GpuGiHeader) == 176);
// GiVolumeBuffer: per probe, 3 channels of 4 half floats.
constexpr VkDeviceSize kGiVolumeBytes = VkDeviceSize{5} * 48 * 48 * 8 * 3 * 8;
constexpr VkDeviceSize kGiProbeBytes = 64;
// GiGroundBuffer: one 48 B record then one 16 B accumulator per ground
// texel, kGiGroundLevels x 256 x 256 of each.
constexpr VkDeviceSize kGiGroundTexelBytes = 48;
constexpr VkDeviceSize kGiGroundAccumBytes = 16;

// Must match the push_constant block in the shaders.
struct PushConstants {
  VkDeviceAddress sceneBuffer = 0;
  // The cluster draw list mesh.mesh reads: phase * kDrawBuckets + bucket.
  uint32_t drawList = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(PushConstants) == 16);

// Must match CloudConstants in clouds_temporal.glsl; the cloud march reads
// only the scene buffer.
struct CloudConstants {
  VkDeviceAddress sceneBuffer = 0;
  // 0 on the first frame at this cloud resolution (no usable history).
  uint32_t historyValid = 0;
  uint32_t inLayer = 0;
  // Effects::cloudOutline (the composite's sharpening).
  float outline = 0.0f;
  uint32_t pad = 0;
};
static_assert(sizeof(CloudConstants) == 24);

// Must match Material in common.glsl.
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
constexpr uint32_t kMaterialAlphaMask = 2u;

struct GpuMaterial {
  float baseColorFactor[4] = {};
  Vec3 emissiveFactor;
  float metallicFactor = 0.0f;
  float roughnessFactor = 0.0f;
  float normalScale = 0.0f;
  float occlusionStrength = 0.0f;
  float alphaCutoff = 0.0f;
  uint32_t baseColorTexture = kNoTexture;
  uint32_t metallicRoughnessTexture = kNoTexture;
  uint32_t normalTexture = kNoTexture;
  uint32_t occlusionTexture = kNoTexture;
  uint32_t emissiveTexture = kNoTexture;
  uint32_t flags = 0;
  float diffuseTransmission = 0.0f;
  uint32_t diffuseTransmissionColorTexture = kNoTexture;
  Vec3 diffuseTransmissionColor;
  float pad = 0.0f;
};
static_assert(sizeof(GpuMaterial) == 96);
static_assert(offsetof(GpuMaterial, diffuseTransmissionColor) == 80);

inline uint32_t textureIndex(const std::optional<TextureId> &texture) {
  return texture ? texture->index : kNoTexture;
}

inline GpuMaterial toGpu(const MaterialData &m) {
  GpuMaterial g;
  std::copy(std::begin(m.baseColorFactor), std::end(m.baseColorFactor),
            g.baseColorFactor);
  g.emissiveFactor = m.emissiveFactor;
  g.metallicFactor = m.metallicFactor;
  g.roughnessFactor = m.roughnessFactor;
  g.normalScale = m.normalScale;
  g.occlusionStrength = m.occlusionStrength;
  g.baseColorTexture = textureIndex(m.baseColorTexture);
  g.metallicRoughnessTexture = textureIndex(m.metallicRoughnessTexture);
  g.normalTexture = textureIndex(m.normalTexture);
  g.occlusionTexture = textureIndex(m.occlusionTexture);
  g.emissiveTexture = textureIndex(m.emissiveTexture);
  g.alphaCutoff = m.alphaCutoff;
  g.flags = m.alphaMode == AlphaMode::Mask ? kMaterialAlphaMask : 0u;
  g.diffuseTransmission = m.diffuseTransmission;
  g.diffuseTransmissionColorTexture = textureIndex(m.diffuseTransmissionColorTexture);
  g.diffuseTransmissionColor = m.diffuseTransmissionColor;
  return g;
}
constexpr VkShaderStageFlags kScenePushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_MESH_BIT_EXT |
    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

struct FrameData {
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence renderFence = VK_NULL_HANDLE;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
  Buffer sceneBuffer;
  // CPU-written when instances change: the instance table's upload source
  // and the hand-placed instances' TLAS records (copied into
  // Renderer::Impl::tlasRecords). Per frame because the CPU may write them
  // while the other frame's commands still read its own.
  Buffer instanceStaging;
  Buffer instanceBuffer;
  // This frame's DrawCounters, copied back for statistics.
  Buffer drawCountReadback;
  // GpuWeather::history: kWeatherHistoryCapacity WeatherHistorySamples.
  Buffer weatherHistory;
  // The streamed slots to update this frame (StreamUpdate), CPU-written;
  // per frame so the CPU never overwrites a list an earlier frame still reads.
  Buffer streamUpdates;
};

// The NVIDIA driver leaks a few KB during device creation and on the first
// acceleration structure build in each command buffer. LeakSanitizer ignores
// allocations made while one of these lives; elsewhere it's a no-op.
#ifdef MERIDIAN_HAS_LSAN
using DriverLeakScope = __lsan::ScopedDisabler;
#else
struct DriverLeakScope {};
#endif

} // namespace meridian
