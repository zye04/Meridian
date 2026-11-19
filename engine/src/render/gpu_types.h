// GPU-side constants and data layouts. Structs marked "must match" mirror
// GLSL declarations in engine/shaders; change both together.
#pragma once

#include <meridian/renderer.h>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
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
constexpr uint32_t kMaxPassTimes = 16;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// 16-bit float keeps smooth gradients across the full HDR range.
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Screen-space motion vectors for temporal anti-aliasing (and later DLSS).
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
// Halton(2,3) jitter cycle length.
constexpr uint32_t kJitterPhases = 8;
// Exposure for the very first frame, before any histogram exists: a sunny day.
constexpr float kInitialEv100 = 15.0f;
// Capacities of the bindless material and texture tables.
constexpr uint32_t kMaxMaterials = 16384;
constexpr uint32_t kMaxTextures = 16384;
constexpr uint32_t kDfgLutSize = 128;
// Depth pyramid levels: enough for a 32768-pixel-wide screen.
constexpr uint32_t kMaxPyramidLevels = 16;
// Shared geometry pools (bump-allocated; real streaming will need an
// allocator with frees): 8M vertices = 384 MB, 32M indices = 128 MB; cluster
// hierarchies: 1M clusters = 80 MB, 32M vertex references and 32M
// triangles = 128 MB each.
constexpr uint32_t kVertexPoolCapacity = 8u << 20;
constexpr uint32_t kIndexPoolCapacity = 32u << 20;
constexpr uint32_t kMaxMeshes = 65536;
constexpr uint32_t kMaxClusters = 1u << 20;
constexpr uint32_t kClusterVertexCapacity = 32u << 20;
constexpr uint32_t kClusterTriangleCapacity = 32u << 20;
// Cluster draw lists: opaque, opaque double-sided, masked, masked
// double-sided. Blended meshes (kDrawBucketBlended) are drawn from the CPU,
// sorted back to front.
constexpr uint32_t kDrawBuckets = 4;
// Per culling phase: work items (up to 64 clusters of one instance each) and
// drawn clusters per draw list; clusters to retest in phase 1. Overflow is
// dropped and counted, and the counts that size indirect dispatches stay at
// capacity (the device must allow dispatches that large).
constexpr uint32_t kMaxWorkItems = 1u << 19;
constexpr uint32_t kMaxDrawnClusters = 1u << 20;
constexpr uint32_t kMaxRetestClusters = 1u << 20;
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
  // Bottom-level acceleration structure: the mesh's triangles for ray tracing.
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
  float pad[3] = {};
};
static_assert(sizeof(GpuAtmosphere) == 112);

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
  float pad[3] = {};
};
static_assert(sizeof(GpuTerrainLayer) == 32);

// Must match Terrain in common.glsl. Layers: soil, needles, rock.
struct GpuTerrain {
  GpuTerrainLayer layers[3];
  uint32_t enabled = 0;
  uint32_t pad[3] = {};
};
static_assert(sizeof(GpuTerrain) == 112);

inline GpuTerrainLayer toGpu(const TerrainLayer &layer) {
  return {layer.baseColor.index, layer.normal.index, layer.arm.index,
          layer.height.index, layer.tileSize};
}

// Must match SceneBuffer in the shaders (std430: each vec3 takes 16 bytes).
struct SceneData {
  Mat4 viewProjection;
  Mat4 currentViewProjection;
  Mat4 previousViewProjection;
  Vec3 sunDirection;
  float pad0 = 0.0f;
  Vec3 sunIlluminance;
  uint32_t frameIndex = 0;
  Vec3 cameraPosition;
  float sunAngularRadius = 0.0f;
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
};
static_assert(sizeof(SceneData) == 816);

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
  uint32_t pad = 0;
};
static_assert(sizeof(MeshInfo) == 48);

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
  uint32_t pad[2] = {};
};
static_assert(sizeof(ClusterInfo) == 80);

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
  uint32_t pad[3];
};
static_assert(sizeof(DrawCounters) == 160);

// Must match InstanceData in common.glsl.
struct InstanceData {
  Mat4 transform;
  uint32_t meshIndex = 0;
  uint32_t pad[3] = {};
};
static_assert(sizeof(InstanceData) == 80);

// Must match depth_pyramid.comp.
struct PyramidConstants {
  int32_t sourceSize[2] = {};
  int32_t destinationSize[2] = {};
  uint32_t level = 0;
  uint32_t pad[3] = {};
};
static_assert(sizeof(PyramidConstants) == 32);

// Must match CullConstants in cull.glsl; shared by cull.comp and
// cluster_cull.comp.
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
};
static_assert(sizeof(CullConstants) == 64);
constexpr uint32_t kCullRetest = 2;

// TLAS instance masks: shadow rays only test shadow casters.
constexpr uint8_t kShadowCasterMask = 0x01;
constexpr uint8_t kNonCasterMask = 0x02;

// Must match ExposureBuffer in exposure.glsl.
struct ExposureState {
  float exposure = 0.0f;
  float ev100 = 0.0f;
};

constexpr uint32_t kHistogramBins = 256;

// Must match the push_constant block in post.glsl.
struct PostConstants {
  uint32_t historyValid = 0;
  float deltaSeconds = 0.0f;
  VkDeviceAddress histogram = 0;
  VkDeviceAddress exposure = 0;
  float exposureCompensation = 0.0f;
};
static_assert(sizeof(PostConstants) == 32);

// Must match the push_constant block in the shaders.
struct PushConstants {
  VkDeviceAddress sceneBuffer = 0;
  // The cluster draw list mesh.mesh reads: phase * kDrawBuckets + bucket.
  uint32_t drawList = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(PushConstants) == 16);

// Must match Material in common.glsl.
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
constexpr uint32_t kMaterialTerrain = 1u;
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
  uint32_t pad[2] = {};
};
static_assert(sizeof(GpuMaterial) == 80);

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
  g.flags = (m.terrain ? kMaterialTerrain : 0u) |
            (m.alphaMode == AlphaMode::Mask ? kMaterialAlphaMask : 0u);
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
  // Top-level acceleration structure over all instances, rebuilt every
  // frame from the TLAS instance buffer.
  Buffer instanceBuffer;
  AccelerationStructure tlas;
  // CPU-visible copy of the instance table, uploaded when instances change.
  Buffer instanceStaging;
  Buffer tlasScratch;
  // This frame's DrawCounters, copied back for statistics.
  Buffer drawCountReadback;
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
