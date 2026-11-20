// GPU-side constants and data layouts. Structs marked "must match" mirror
// GLSL declarations in engine/shaders; change both together.
#pragma once

#include <meridian/placement.h>
#include <meridian/renderer.h>

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
constexpr uint32_t kMaxPassTimes = 16;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// 16-bit float keeps smooth gradients across the full HDR range.
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Screen-space motion vectors for temporal anti-aliasing (and later DLSS).
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kTransparencyMaskFormat = VK_FORMAT_R8_UNORM;
// Exposure for the very first frame, before any histogram exists: a sunny day.
constexpr float kInitialEv100 = 15.0f;
// Capacities of the bindless material and texture tables.
constexpr uint32_t kMaxMaterials = 16384;
constexpr uint32_t kMaxTextures = 16384;
constexpr uint32_t kDfgLutSize = 128;
// Depth pyramid levels: enough for a 32768-pixel-wide screen.
constexpr uint32_t kMaxPyramidLevels = 16;
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
  // The camera position wrapped by the layer's period (renderer_frame.cpp).
  float offset[3] = {};
  float occlusionStrength = 1.0f;
};
static_assert(sizeof(GpuTerrainLayer) == 36);

// Terrain texture coordinates (mesh.frag) are the camera-relative position
// plus the camera position wrapped by a period, so they stay small and
// precise anywhere on the map. Layers repeat every kTerrainWrapTiles tiles;
// along z, the hex-tiled layers repeat every kTerrainHexWrapRows rows of
// hex cells (sqrt(3)/4 tiles each, so 256.3 tiles), since no whole number
// of tiles is a whole number of rows. The soil/needles mix noise repeats
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
static_assert(kTerrainNoiseWrapCells * 27 % 10 == 0,
              "the mix noise's 2.7x octave needs a whole number of cells per wrap");

// CDLOD terrain quadtree (terrain.glsl): level 0 nodes are 16 m, 32x32
// quads of 0.5 m; each level doubles. The root covers 1049 km around the
// origin: the horizon up to ~20 km altitude. Level 0's LOD range in meters;
// each level's range doubles.
constexpr uint32_t kTerrainRootLevel = 16;
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

// Must match Terrain in common.glsl. Layers: soil, needles, rock.
struct GpuTerrain {
  GpuTerrainLayer layers[3];
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
};
static_assert(sizeof(GpuTerrain) == 256);

inline GpuTerrainLayer toGpu(const TerrainLayer &layer) {
  return {layer.baseColor.index, layer.normal.index, layer.arm.index,
          layer.height.index, layer.tileSize, {}, layer.occlusionStrength};
}

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
  float pad2 = 0.0f;
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
  uint32_t placedCount = 0;
  // Cells: the largest layer draw distance (0: some layer has none) and how
  // far instances' bounds reach past their cell horizontally.
  float cellDrawDistance = 0.0f;
  float cellMargin = 0.0f;
  uint32_t pad3 = 0;
  // Every mesh's cluster group BVH nodes and DAG levels.
  VkDeviceAddress clusterNodes = 0;
  VkDeviceAddress clusterLevels = 0;
};
static_assert(sizeof(SceneData) == 1056);

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
  uint32_t pad[3] = {};
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
// transitionSize: projected size below which it is drawn as an impostor
// (0: never). maxDrawDistance: the largest of its layers' (0: none).
struct PrototypeInfo {
  uint32_t firstMesh = 0;
  uint32_t meshCount = 0;
  uint32_t pad[2] = {}; // std430 aligns the vec3 to 16 bytes
  Vec3 boundsCenter;
  float boundsRadius = 0.0f;
  VkDeviceAddress blasAddress = 0;
  float transitionSize = 0.0f;
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
};
static_assert(sizeof(DrawCounters) == (52 + 8 * kBvhDepths) * 4);
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

// Must match the push constants in scan.comp.
struct ScanConstants {
  VkDeviceAddress values = 0;
  uint32_t count = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(ScanConstants) == 16);

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
  float deltaSeconds = 0.0f;
  VkDeviceAddress histogram = 0;
  VkDeviceAddress exposure = 0;
  float exposureCompensation = 0.0f;
};
static_assert(sizeof(PostConstants) == 32);

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
  g.flags = m.alphaMode == AlphaMode::Mask ? kMaterialAlphaMask : 0u;
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
