// Private state of meridian::Renderer, shared by renderer_setup.cpp,
// renderer_resources.cpp, renderer_terrain.cpp, renderer_placement.cpp and
// renderer_frame.cpp.
#pragma once

#include "gpu_types.h"
#include "vulkan_util.h"

#include <meridian/placement.h>
#include <meridian/renderer.h>
#include <meridian/upscaler.h>
#include <meridian/upscaler_vk.h>
#include <meridian/window.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace meridian {

// Mismatch counters of the Debug checks; label names the check in its
// messages.
struct RunStats {
  const char *label;
  uint32_t evaluated = 0, kept = 0, mismatches = 0;
  float worstHeight = 0.0f;
  // Point UINT32_MAX: the run as a whole; layer UINT32_MAX: the cell record.
  void report(int32_t cellX, int32_t cellZ, uint32_t layer, uint32_t point) {
    if (mismatches++ == 0) {
      std::printf("%s: first mismatch at cell (%d, %d), layer %d, point %d\n", label, cellX,
                  cellZ, static_cast<int>(layer), static_cast<int>(point));
    }
  }
};

// One (cell, layer) run of GPU instances against evaluatePoint (mirrored
// passed through): keep decisions, prototypes and x/z exactly (order
// included), the float attributes within tolerance. tags (optional, streamed
// runs): the dither thresholds, which must equal the pattern's with the
// layer index above.
void compareRun(const Heightfield &field, const GeoChannels *geo, const PlacementLayer &layer,
                const DitherPattern &pattern, uint32_t seed, int32_t cellX, int32_t cellZ,
                uint32_t layerIndex, const PlacedInstance *run, uint32_t count,
                const uint32_t *tags, RunStats &stats, bool mirrored = false);

// setPlacement's GPU placement inputs, kept for the far field's transient
// placement (buildFarField): constants has the inputs set (grid, counts,
// instances and cells are per dispatch); the far-field layers are the first
// layerCount, pointsPerCell their pattern points per cell.
struct FarPlacement {
  PlacementConstants constants;
  VkPipeline countPipeline = VK_NULL_HANDLE, writePipeline = VK_NULL_HANDLE,
             scanPipeline = VK_NULL_HANDLE;
  VkPipelineLayout placementLayout = VK_NULL_HANDLE, scanLayout = VK_NULL_HANDLE;
  uint32_t layerCount = 0, pointsPerCell = 0, seed = 0;
  std::span<const DitherPattern> patterns;
};


struct Renderer::Impl {
  GLFWwindow *window = nullptr;
  const Window *windowSize = nullptr;
  vkb::Instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  vkb::Device device;
  // Supplies the optional backends (DLSS); null when none was linked in.
  UpscalerProvider *upscalerProvider = nullptr;
  // Device functions from extensions (ray tracing), which the loader doesn't
  // export directly.
  vkb::DispatchTable dispatch;
  VmaAllocator allocator = nullptr;
  VkDeviceSize scratchAlignment = 0;
  VkDeviceSize maxBufferSize = 0;

  VkQueue graphicsQueue = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily = 0;

  vkb::Swapchain swapchain;
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainImageViews;
  // One per swapchain image: the presentation engine may still be reading a
  // semaphore after our fence signals, so it can't be shared across images.
  std::vector<VkSemaphore> renderFinished;
  bool swapchainOutOfDate = false;

  // The scene renders at renderExtent (the display, swapchain.extent, scaled
  // by aaSettings.renderScale); the upscaler and everything after it run at
  // the display size. All of these images, and the upscaler, are recreated
  // with the swapchain.
  AaSettings aaSettings;
  // Stores the settings, falling back to TAA when the provider lacks them.
  void setAaSettings(const AaSettings &settings);
  VkExtent2D renderExtent{};
  // Render-sized.
  Image depth;
  Image hdr;
  Image motion;
  Image transparencyMask;
  // Scene attachment 6: the sun radiance already in hdr (contact shadows).
  Image sunLight;
  // Only created when the upscaler wants guides (Ray Reconstruction).
  Image diffuseAlbedo;
  Image specularAlbedo;
  Image normalRoughness;
  Image specularHitDistance;
  // Display-sized: the upscaler's output, read by the exposure histogram,
  // the tonemap and .pfm captures. Always in VK_IMAGE_LAYOUT_GENERAL.
  Image output;
  std::unique_ptr<Upscaler> upscaler;
  // Min-reduced depth for occlusion culling: the render size rounded down to
  // powers of two, one view per level. Always in VK_IMAGE_LAYOUT_GENERAL.
  Image depthPyramid;
  std::vector<VkImageView> depthPyramidLevels;
  // Glare mip chain (bloom.comp) at half the display size, display-sized
  // like `output`. Each set reads binding 0 and writes binding 1:
  // bloomDownSets[i] writes level i (from `output` for i = 0),
  // bloomUpSets[i] adds level i + 1 into level i, bloomCompositeSet blends
  // level 0 into `output`.
  Image bloomImage;
  VkExtent2D bloomExtent{};
  std::vector<VkImageView> bloomLevels;
  VkDescriptorPool bloomPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> bloomDownSets;
  std::vector<VkDescriptorSet> bloomUpSets;
  VkDescriptorSet bloomCompositeSet = VK_NULL_HANDLE;
  VkExtent2D depthPyramidExtent{};
  bool historyValid = false;
  Mat4 previousViewProjection;
  Mat4 previousView;
  Vec3 previousCameraPosition;
  // From setWeather; the history is copied to the frame's buffer in draw.
  GpuWind wind;
  GpuWind previousWind;
  GpuWeather weather;
  std::array<WeatherHistorySample, kWeatherHistoryCapacity> weatherHistory{};
  // Wind offset and time from setWeather, the map window from draw.
  GpuClouds clouds;
  // The depth pyramid holds last frame's depth at the current size, so the
  // early cluster cull can test against it.
  bool pyramidValid = false;

  std::array<FrameData, kFramesInFlight> frames;
  uint64_t frameNumber = 0;
  // frameNumber at the last cut: the jitter phase and the noise frame index
  // count from it, so a cut's frames repeat a fresh start's.
  uint64_t cutFrame = 0;

  // Per frame in flight: a start timestamp, then one after each named pass.
  VkQueryPool timestampPool = VK_NULL_HANDLE;
  float timestampPeriodNs = 0.0f;
  float gpuFrameMs = 0.0f;
  std::array<PassTime, kMaxPassTimes> passTimes;
  // The pass count varies with the overlay, so each slot keeps the count of
  // the frame last recorded in it; passTimeCount is that of the frame whose
  // times passTimes holds.
  // Names go with the count: a toggle frame must not pair another frame's
  // names with its times.
  std::array<uint32_t, kFramesInFlight> slotPassCount{};
  std::array<std::array<const char *, kMaxPassTimes>, kFramesInFlight> slotPassNames{};
  uint32_t passTimeCount = 0;
  // beginOverlay() was called since the last draw().
  bool overlayPending = false;
  float cpuWaitMs = 0.0f;
  uint32_t drawnInstances = 0;
  uint32_t drawnClusters = 0;
  uint32_t drawnTriangles = 0;
  // Over the run, main view and cascades (draw reads them back).
  uint32_t droppedEntries = 0;
  std::array<uint32_t, 5> droppedByList{};
  CullNeed mainNeed;
  CullNeed cascadeNeed;
  std::array<CullNeed, kShadowCascades> cascadeNeeds{};
  uint32_t drawnTerrainNodes = 0;
  uint32_t drawnCells = 0;
  uint32_t drawnFarCells = 0;
  uint32_t visitedBvhNodes = 0;
  float lodErrorPixels = 1.0f;
  bool occlusionCulling = true;
  ShadowMode shadowMode = ShadowMode::Raster;
  bool rayTracing = false;
  bool loadPrinted = false;
  // Reference path tracer (renderer_reference.cpp): accumulation per pixel
  // (rgb sum + count, luminance sum of squares), display-sized.
  bool referencing = false;
  ReferenceMode referenceMode = ReferenceMode::Radiance;
  uint32_t referenceCount = 0;
  Buffer referenceSum;
  Buffer referenceSquares;
  VkDescriptorSetLayout referenceSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool referencePool = VK_NULL_HANDLE;
  VkDescriptorSet referenceSet = VK_NULL_HANDLE;
  VkPipelineLayout referenceLayout = VK_NULL_HANDLE;
  VkPipeline referencePipeline = VK_NULL_HANDLE;
  // Each cascade's rendered window, the sun it was rendered for and what
  // plan chose for this frame. shadowStale: instances or placement changed.
  std::array<Image, kShadowCascades> shadowMaps;
  std::array<uint32_t, kShadowCascades> shadowTextures = {};
  std::array<CascadeWindow, kShadowCascades> shadowWindows;
  std::array<bool, kShadowCascades> shadowRendered = {};
  std::array<bool, kShadowCascades> shadowPlanned = {};
  std::array<std::array<Buffer, kShadowCascades>, kFramesInFlight> shadowScenes;
  LightBasis shadowBasis;
  Vec3 shadowSun;
  bool shadowStale = true;
  bool shadowRefresh = false;
  Buffer shadowCounters;
  Buffer shadowDrawLists;
  VkPipeline shadowPipeline = VK_NULL_HANDLE;
  VkPipeline shadowMaskedPipeline = VK_NULL_HANDLE;
  uint32_t shadowRenderCount = 0;
  float shadowMaxMs = 0.0f;
  // The highest world y that casts into a cascade (setTerrain).
  double shadowCasterTop = 2500.0;
  bool voxelFarShadows = true;
  GiMode giMode = GiMode::Off;
  // The GI probe table (gi.glsl), created on the first frame with GI on.
  Buffer giBuffer;
  // The ground cache records and the albedo accumulator (GiGroundBuffer).
  Buffer giGroundBuffer;
  // The probes' SH packed for the shading's reads (GpuGiHeader::volumeBuffer).
  Buffer giVolumeBuffer;
  VkPipeline giProbesPipeline = VK_NULL_HANDLE;
  VkPipeline giGroundPipeline = VK_NULL_HANDLE;
  VkPipeline giAlbedoScatterPipeline = VK_NULL_HANDLE;
  VkPipeline giAlbedoResolvePipeline = VK_NULL_HANDLE;
  // Set 2 of giAlbedoLayout: scene depth and diffuse albedo, sampled.
  VkDescriptorSetLayout giAlbedoSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool giAlbedoPool = VK_NULL_HANDLE;
  VkDescriptorSet giAlbedoSet = VK_NULL_HANDLE;
  VkPipelineLayout giAlbedoLayout = VK_NULL_HANDLE;
  bool giReset = true;

  VkCommandPool uploadPool = VK_NULL_HANDLE;
  VkCommandBuffer uploadCommandBuffer = VK_NULL_HANDLE;
  VkFence uploadFence = VK_NULL_HANDLE;

  // Shared by everything that reads the scene buffer: meshes, the sky and
  // the atmosphere compute passes.
  VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
  VkPipeline meshPipeline = VK_NULL_HANDLE;
  VkPipeline maskedMeshPipeline = VK_NULL_HANDLE;
  VkPipeline maskedDepthPipeline = VK_NULL_HANDLE;
  VkPipeline blendedMeshPipeline = VK_NULL_HANDLE;
  VkPipelineLayout cullLayout = VK_NULL_HANDLE;
  VkPipeline cellCullPipeline = VK_NULL_HANDLE;
  VkPipeline cullPipeline = VK_NULL_HANDLE;
  VkPipeline clusterTraversePipeline = VK_NULL_HANDLE;
  VkPipeline clusterCullPipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout cullSetLayout = VK_NULL_HANDLE;
  // The depth pyramid, sampled through minSampler.
  VkDescriptorSet cullSet = VK_NULL_HANDLE;
  // Contact shadows: set 0 = depth, sun light, HDR (storage); rewritten
  // with the swapchain's images.
  VkDescriptorSetLayout contactSetLayout = VK_NULL_HANDLE;
  VkDescriptorSet contactSet = VK_NULL_HANDLE;
  VkPipelineLayout contactLayout = VK_NULL_HANDLE;
  VkPipeline contactPipeline = VK_NULL_HANDLE;
  // Blended instances sorted back to front each frame (reused storage).
  std::vector<std::pair<float, uint32_t>> blendedDraws;

  // GPU scene: shared geometry pools, mesh and instance tables, and the
  // cluster draw lists the cull passes write.
  Buffer vertexPool;
  Buffer indexPool;
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  Buffer meshTable;
  Buffer instanceTable;
  std::vector<InstanceData> instances;
  // The instance table and the TLAS are built on the first frame and again
  // after instances change.
  bool instancesDirty = true;
  // Top-level acceleration structure over all instances, shared by both
  // frames in flight, and its instance records: the placed instances'
  // (tlas_fill.comp, at setPlacement), then the hand-placed ones' (copied
  // from FrameData::instanceBuffer at each build). Sized for placedCount +
  // kMaxInstances records. Only with rayTracing.
  AccelerationStructure tlas;
  Buffer tlasScratch;
  Buffer tlasRecords;
  // Cluster hierarchies of every mesh, bump-allocated like the pools above.
  Buffer clusterPool;
  Buffer clusterVertexPool;
  Buffer clusterTrianglePool;
  uint32_t clusterCount = 0;
  uint32_t clusterVertexCount = 0;
  uint32_t clusterTriangleCount = 0;
  // Their group BVHs and DAG levels; traversalDepths: dispatches per phase
  // that reach the deepest leaf.
  Buffer clusterNodePool;
  Buffer clusterLevelPool;
  uint32_t clusterNodeCount = 0;
  uint32_t clusterLevelCount = 0;
  uint32_t traversalDepths = 0;
  // The voxel clusters' bricks (GpuVoxelBrick), voxels (PackedVoxel) and
  // axis depth words, bump-allocated too; voxelPoolInfo holds their
  // addresses (GpuVoxelPools, SceneData.voxelPools), rewritten when one
  // grows. voxelInstanceList: the voxel lists, made with the first voxels.
  Buffer voxelBrickPool;
  Buffer voxelPool;
  Buffer voxelAxisPool;
  Buffer voxelPoolInfo;
  Buffer voxelInstanceList;
  uint32_t voxelBrickCount = 0;
  uint32_t voxelCount = 0;
  // Culling output, both phases: instance chunks for the instance cull, BVH
  // nodes for the traversal, work items for the cluster cull, cluster draw
  // lists, and the DrawCounters (indirect arguments and statistics).
  Buffer chunks;
  // The BVH traversal's three node queues (see cull.glsl).
  Buffer nodeQueues;
  Buffer workItems;
  Buffer drawLists;
  // Clusters the early cull found occluded, retested by the late cull.
  Buffer retestClusters;
  Buffer drawCounters;
  // Visible at the end of last frame's culling (1) or not: per instance id
  // (kPlacedBase + placedCount + the ring's of them), then per placement
  // cell and streamed record.
  Buffer visibility;

  // Terrain (renderer_terrain.cpp): the heightfield, its normal map (both
  // in the bindless table) and min/max pyramid (empty until setTerrain),
  // the CDLOD selection's counters (TerrainCounters) and node lists: two
  // ping-pong selection lists, then the draw list.
  Image terrainHeightImage;
  Image terrainNormalImage;
  Buffer terrainPyramid;
  // Trilinear, anisotropic, mirrored repeat like the heightfield itself.
  VkSampler terrainSampler = VK_NULL_HANDLE;
  Buffer terrainCounters;
  Buffer terrainLists;
  VkPipelineLayout terrainSelectLayout = VK_NULL_HANDLE;
  VkPipeline terrainSelectPipeline = VK_NULL_HANDLE;
  VkPipeline terrainPipeline = VK_NULL_HANDLE;

  // A copy of setTerrain's heightfield: placement reads its samples.
  Heightfield heightfield;
  // setGeoChannels: the channels on the CPU (the Debug checks' reference)
  // and on the GPU (a uvec2 per texel).
  std::optional<GeoChannels> geo;
  Buffer geoBuffer;
  // The forest floor (TerrainMaterial::forestLayerId): setPlacement bakes
  // that layer's density per geo texel into forestFloorImage
  // (terrain.forestTexture).
  uint32_t forestLayerId = 0;
  Image forestFloorImage;
  // Placement (renderer_placement.cpp): the placed table (24-bit ids from
  // kPlacedBase) and one Cell per grid cell; empty until setPlacement.
  Buffer placedInstances;
  Buffer cells;
  PlacementGrid placementGrid{};
  // The static placed instances (ids kPlacedBase + [0, placedCount), the
  // ones in the TLAS); the streamed ring's ids follow.
  uint32_t placedCount = 0;
  // Streamed placement (renderer_placement.cpp), while a layer is streamed:
  // the placement inputs setPlacement uploaded (kept for the per-frame
  // updates), the ring's thresholds, the records' coordinates and the cells
  // the slots hold.
  struct Stream {
    Buffer heights, layers, points, picks, bounds;
    Buffer layerTable, thresholds, coords;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t layerCount = 0;
    uint32_t window = 0;
    // Ring instances per slot (the layers' pattern sizes, summed) and in all.
    uint32_t runStride = 0;
    uint32_t ringCount = 0;
    // Streamed records after the grid's cells in `cells`, and the grid's.
    uint32_t recordCount = 0;
    uint32_t gridCells = 0;
    uint32_t seed = 0;
    // The cell each slot holds; INT32_MIN: none yet.
    std::vector<std::array<int32_t, 2>> held;
    // Debug check inputs: the streamed layers, their patterns, the
    // prototype spheres.
    std::vector<PlacementLayer> layerList;
    std::vector<DitherPattern> patterns;
    std::vector<std::array<float, 4>> spheres;
    bool active() const { return layerCount > 0; }
  } stream;
  // PrototypeInfo per prototype, the mesh lists they index, and their BLAS
  // (one per prototype, a geometry per mesh, compacted).
  Buffer prototypeTable;
  Buffer prototypeMeshTable;
  std::vector<AccelerationStructure> prototypeBlas;
  float cellDrawDistance = 0.0f;
  float cellMargin = 0.0f;
  // Far field (renderer_farfield.cpp): per level the brick columns and
  // voxel pools, FarCell per cell, the canopy top pyramid, the albedo
  // palette and the GpuFarField pointing at them; empty unless placement
  // has far-field layers.
  std::array<Buffer, kFarLevels> farColumns;
  std::array<Buffer, kFarLevels> farVoxels;
  Buffer farCells;
  Buffer farPyramid;
  Buffer farPalette;
  Buffer farFieldInfo;
  // The far cells the cell cull appends each frame (one uint per cell).
  Buffer farCellList;
  // The far shadow map (renderer_farshadow.cpp): two maps of 16-bit depths,
  // the built one (index farShadowBuilt) and the one under rebuild, each
  // with the grid it was built for; farShadowState is farShadowStep's.
  FarShadowBox farShadowBox;
  uint32_t farShadowCapacity = 0;
  std::array<Buffer, 2> farShadowMaps;
  std::array<FarShadowGrid, 2> farShadowGrids;
  uint32_t farShadowBuilt = 0;
  FarShadowState farShadowState;
  FarShadowStep farShadowPending;
  bool farShadowReady = false;
  VkPipelineLayout farShadowLayout = VK_NULL_HANDLE;
  VkPipeline farShadowPipeline = VK_NULL_HANDLE;
  // The terrain shadow-height map (renderer_terrain_shadow.cpp) and the
  // sun and window it was last built for.
  Buffer terrainShadowMap;
  VkPipelineLayout terrainShadowLayout = VK_NULL_HANDLE;
  VkPipeline terrainShadowPipeline = VK_NULL_HANDLE;
  TerrainShadowConstants terrainShadowBuild;
  Vec3 terrainShadowSun{};
  bool terrainShadowValid = false;
  bool terrainShadowPending = false;
  VkPipeline voxelPipeline = VK_NULL_HANDLE;
  VkPipeline voxelInstancePipeline = VK_NULL_HANDLE;
  VkPipeline voxelBricksPipeline = VK_NULL_HANDLE;
  VkPipeline farShadowCasterPipeline = VK_NULL_HANDLE;
  // Voxel clusters' quads into a cascade (voxel_instance.mesh, depth only).
  VkPipeline voxelShadowCasterPipeline = VK_NULL_HANDLE;
  VoxelLod voxelLod = VoxelLod::On;
  float voxelNear = 0.0f;
  float farSwitchPixels = kFarSwitchPixels;
  float farExtent = kFarExtent;
  // The far field's cells (0: none): cell_cull's threads past the
  // hand-placed chunks.
  uint32_t farGridCells = 0;
  VoxelCoverage voxelCoverage = VoxelCoverage::AreaPixelPhase;
  uint32_t drawnVoxelTrees = 0;
  uint32_t drawnVoxelBricks = 0;
  uint32_t drawnVoxelQuads = 0;
  double drawnVoxelArea = 0.0;
  uint32_t drawnSubpixelVoxels = 0;

  VkSampler nearestSampler = VK_NULL_HANDLE;
  VkSampler linearSampler = VK_NULL_HANDLE;
  // Linear filtering that returns the minimum of the footprint.
  VkSampler minSampler = VK_NULL_HANDLE;
  VkSampler shadowSampler = VK_NULL_HANDLE; // linear depth compare, for the cascades
  VkDescriptorSetLayout pyramidSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool pyramidPool = VK_NULL_HANDLE;
  // pyramidSets[i] reads level i - 1 (the depth image for i = 0), writes i.
  std::array<VkDescriptorSet, kMaxPyramidLevels> pyramidSets{};
  VkPipelineLayout pyramidLayout = VK_NULL_HANDLE;
  VkPipeline pyramidPipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet postSet = VK_NULL_HANDLE;
  VkPipelineLayout postLayout = VK_NULL_HANDLE;
  VkPipeline histogramPipeline = VK_NULL_HANDLE;
  VkPipeline exposurePipeline = VK_NULL_HANDLE;
  VkPipeline skyPipeline = VK_NULL_HANDLE;

  // Atmosphere LUTs (Hillaire 2020), always in GENERAL layout: written by
  // compute passes, sampled by the sky, terrain and later passes.
  Image transmittanceLut;
  Image multiScatteringLut;
  Image skyViewLut;
  Image aerialPerspectiveLut;
  VkDescriptorSetLayout atmosphereSetLayout = VK_NULL_HANDLE;
  VkDescriptorSet atmosphereSet = VK_NULL_HANDLE;
  VkPipeline transmittanceLutPipeline = VK_NULL_HANDLE;
  VkPipeline multiScatteringLutPipeline = VK_NULL_HANDLE;
  VkPipeline skyViewLutPipeline = VK_NULL_HANDLE;
  VkPipeline aerialPerspectiveLutPipeline = VK_NULL_HANDLE;
  VkPipeline skyIrradiancePipeline = VK_NULL_HANDLE;
  Buffer skyIrradianceBuffer;
  // The transmittance and multiple-scattering LUTs depend only on the
  // atmosphere; they're rebuilt when it changes.
  std::optional<Atmosphere> builtAtmosphere;

  // Materials and textures, bound once per frame as set 1 ("bindless").
  Buffer materialBuffer;
  uint32_t materialCount = 0;
  std::vector<Image> textures;
  // CPU copy of what draw ordering and acceleration structures need.
  std::vector<MaterialData> materials;
  GpuTerrain terrain;
  Image dfgLut;
  VkSampler textureSampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool materialPool = VK_NULL_HANDLE;
  VkDescriptorSet materialSet = VK_NULL_HANDLE;

  // Auto-exposure: a luminance histogram and the adapted exposure, both
  // GPU-only. Each frame shades with the exposure the previous frame computed.
  Buffer histogramBuffer;
  Buffer exposureBuffer;
  VkPipeline tonemapPipeline = VK_NULL_HANDLE;

  // Grade + AgX as a kGradeLutSize^3 LUT in GENERAL layout, sampled by the
  // tonemap (post binding 1), rebaked (binding 2) when the grade changes.
  // gradeLayout is the post set plus GradeConstants.
  Grade grade;
  bool gradeDirty = true;
  Bloom bloom;
  Effects effects;
  VkDescriptorSetLayout bloomSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout bloomLayout = VK_NULL_HANDLE;
  VkPipeline bloomDownPipeline = VK_NULL_HANDLE;
  VkPipeline bloomUpPipeline = VK_NULL_HANDLE;
  VkPipeline bloomCompositePipeline = VK_NULL_HANDLE;
  Image gradeLut;
  Buffer gradeBuffer;
  VkPipelineLayout gradeLayout = VK_NULL_HANDLE;
  VkPipeline gradeLutPipeline = VK_NULL_HANDLE;

  // Clouds (renderer_clouds.cpp), in the bindless table: noise volumes
  // generated at load, and the weather map rewritten every frame. All four
  // stay in GENERAL layout, matching both the storage set the generators
  // write through and the bindless descriptors.
  Image cloudBaseNoise;
  Image cloudDetailNoise;
  Image cloudCurlNoise;
  Image weatherMap;
  // The cloud shadow map (RGBA16F, GENERAL): the march's output and its
  // blurred copy, which the bindless table holds.
  Image cloudShadowRaw;
  Image cloudShadowMap;
  // The sky capture (RGBA16F, GENERAL): clouds around the camera,
  // octahedral, written through the march sets (binding 9).
  Image skyCapture;
  // Level 0 of each noise volume, for storage writes.
  std::array<VkImageView, 2> cloudNoiseLevel0{};
  VkDescriptorSetLayout cloudSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool cloudPool = VK_NULL_HANDLE;
  VkDescriptorSet cloudSet = VK_NULL_HANDLE;
  VkPipelineLayout cloudLayout = VK_NULL_HANDLE;
  VkPipeline weatherMapPipeline = VK_NULL_HANDLE;
  VkPipeline cloudShadowPipeline = VK_NULL_HANDLE;
  VkPipeline cloudShadowBlurPipeline = VK_NULL_HANDLE;
  // Cloud march output, [0] quarter and [1] half resolution (inside the
  // layer): luminance + transmittance and mean cloud depth, sized like the
  // swapchain and recreated with it, always in GENERAL layout; and the
  // temporal history of both, ping-ponged ([resolution][frame parity]).
  // One set per resolution and parity (scene depth, the march targets, the
  // history to read and the one to write, HDR and motion); the march,
  // resolve and composite share the layout.
  std::array<Image, 2> cloudColor;
  std::array<Image, 2> cloudDepth;
  std::array<std::array<Image, 2>, 2> cloudHistory;
  std::array<std::array<Image, 2>, 2> cloudHistoryDepth;
  VkDescriptorSetLayout cloudMarchSetLayout = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> cloudMarchSets{};
  VkPipelineLayout cloudMarchLayout = VK_NULL_HANDLE;
  VkPipeline cloudMarchPipeline = VK_NULL_HANDLE;
  VkPipeline cloudResolvePipeline = VK_NULL_HANDLE;
  VkPipeline cloudCompositePipeline = VK_NULL_HANDLE;
  VkPipeline skyCapturePipeline = VK_NULL_HANDLE;
  bool cloudsEnabled = true;
  // The resolution last frame's history was resolved at (none yet: 2).
  uint32_t cloudHistoryResolution = 2;
  // Bottom and top (m) of the cloud layer at the camera, from setWeather.
  float cameraCloudLayer[2] = {};
  // Last frame's cloud wind offset modulo a short period, for the
  // displacement since then.
  float cloudWindLocal[2] = {};
  bool cloudWindLocalValid = false;
  // The cloud wind's displacement summed since startup (x, z), in double:
  // the cloud shadow map's texel grid moves with it.
  double cloudWindTravel[2] = {};
  // The cloud shadow map's texel indices of the camera in the cloud frame,
  // last frame (placeCloudShadow); a change re-marches the whole map.
  int64_t cloudShadowSnap[2] = {};
  bool cloudShadowFullRefresh = true;

  std::vector<GpuMesh> meshes;
  // Per mesh, its far-field template voxels (ClusterLod::farBricks and
  // farVoxels; empty unless it voxelizes), for buildFarField.
  struct MeshFarVoxels {
    std::vector<VoxelBrickRef> bricks;
    std::vector<PackedVoxel> voxels;
  };
  std::vector<MeshFarVoxels> meshFarVoxels;

  std::optional<std::filesystem::path> capturePath;
  std::optional<std::filesystem::path> albedoCapturePath;
  bool albedoCapture = false;

  void createSwapchain();
  void ensureDiffuseAlbedo();
  void destroySwapchain();
  void createBloomChain();
  void createFrames();
  void createDescriptors();
  void createPipelines();
  void createExposureBuffers();
  void createScene();
  void createAtmosphere();
  void createGrade();
  void createMaterialSystem();
  void computeDfgLut();
  void createClouds();
  void createReference();
  void destroyReference();
  void resetReference();
  void dispatchReference(VkCommandBuffer cmd, const Camera &camera, VkDeviceAddress scene);
  void destroyClouds();
  void createGi();
  void destroyGi();
  void writeGiAlbedoSet();
  void drawGiProbes(VkCommandBuffer cmd, Vec3 cameraPosition, uint32_t frameIndex);
  void drawGiGround(VkCommandBuffer cmd);
  void drawGiAlbedo(VkCommandBuffer cmd, VkDeviceAddress sceneBuffer);
  void placeWeatherMap(Vec3 cameraPosition);
  void drawWeatherMap(VkCommandBuffer cmd, const FrameData &frame);
  void placeCloudShadow(Vec3 cameraPosition, Vec3 sunDirection);
  void drawCloudShadow(VkCommandBuffer cmd, const FrameData &frame);
  void drawSkyCapture(VkCommandBuffer cmd, const FrameData &frame);
  void createCloudTargets();
  void destroyCloudTargets();
  bool cameraInCloudLayer(Vec3 cameraPosition) const;
  void drawClouds(VkCommandBuffer cmd, const FrameData &frame, Vec3 cameraPosition);

  Image createImage(VkFormat format, VkExtent3D extent,
                    VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    uint32_t mipLevels = 1);
  Image createRenderImage(VkFormat format, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect);
  Image createDisplayImage(VkFormat format, VkImageUsageFlags usage,
                           VkImageAspectFlags aspect);
  void destroyImage(Image &image);

  Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaAllocationCreateFlags flags,
                      VkDeviceSize alignment = 0);
  void destroyBuffer(Buffer &buffer);
  // A buffer at least 1.5x larger than `pool` that holds `bytes`, or none
  // if `pool` already does.
  Buffer grownPool(const Buffer &pool, VkDeviceSize bytes, VkBufferUsageFlags usage);
  // Shrinks every mesh pool to the bytes in use (growth leaves up to a
  // third unused), before placement and the far field allocate.
  void trimPools();
  Buffer uploadStorage(const void *data, VkDeviceSize bytes);
  void writeTexture(uint32_t index, VkImageView view, VkSampler sampler,
                    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  void uploadTerrainHeights(const Heightfield &field);
  void buildTerrainNormals(const Heightfield &field);

  AccelerationStructure
  createAccelerationStructure(VkAccelerationStructureTypeKHR type,
                              VkDeviceSize size);
  void destroyAccelerationStructure(AccelerationStructure &as);
  // One geometry per mesh (a hit's geometry index is the position in
  // `meshIndices`), compacted. builtBytes: the size before compaction.
  AccelerationStructure buildBlas(std::span<const uint32_t> meshIndices,
                                  VkDeviceSize *builtBytes = nullptr);
  // (Re)creates the TLAS and its scratch and record buffers for `capacity`
  // instances.
  void createTlas(uint32_t capacity);
  // (Re)creates the visibility buffer for the current placement, all
  // invisible.
  void createVisibility();
  void buildTlas(VkCommandBuffer cmd, FrameData &frame);
  void buildPlacedTlas(std::span<const Prototype> prototypes,
                       std::span<const PlacementLayer> layers,
                       const std::vector<std::array<float, 4>> &bounds);
  // The far field of the current placement: prototype templates, then the
  // splat of the far-field layers over the far grid, from transient
  // instances placed again (beyond the crop mirrored) slice by slice
  // (offsets: setPlacement's per (cell, layer), for the trees per cell).
  void buildFarField(std::span<const Prototype> prototypes,
                     std::span<const PlacementLayer> layers,
                     const std::vector<uint32_t> &offsets, const FarPlacement &placement);
  void destroyFarField();
  // Returns the maps' bytes (0: no forest).
  VkDeviceSize createFarShadow(const FarFieldGrid &grid, const std::vector<FarCell> &farCells);
  void destroyFarShadow();
  void dispatchFarShadow(VkCommandBuffer cmd, uint32_t index, uint32_t first, uint32_t count);
  void updateFarShadow(Vec3 sun, SceneData &scene);
  void recordFarShadow(VkCommandBuffer cmd);
  void checkFarShadow(const FarFieldCpu &cpu);
  void updateTerrainShadow(Vec3 sun, Vec3 cameraPosition, VkDeviceAddress sceneAddress,
                           SceneData &scene);
  void recordTerrainShadow(VkCommandBuffer cmd);
  void destroyTerrainShadow();
  // Far cells as their voxels (voxel.mesh/frag), in the early scene pass.
  void drawFarField(VkCommandBuffer cmd);
  void uploadInstances(VkCommandBuffer cmd, FrameData &frame);
  // Streamed placement: writes the slots whose cell changed for `camera`
  // into frame.streamUpdates (all of them after setPlacement) and returns
  // how many; recordStreamUpdate then places them. Debug runs the update on
  // its own before the frame and checks it against evaluatePoint.
  uint32_t planStreamUpdates(const Vec3 &camera, FrameData &frame);
  void recordStreamUpdate(VkCommandBuffer cmd, const FrameData &frame, uint32_t updates);
  void checkStreamUpdate(const FrameData &frame, uint32_t updates);
  void destroyStreamBuffers(Stream &stream);
  void destroyStream();
  void resetDrawCounters(VkCommandBuffer cmd, const Buffer &counters);
  // Writes the voxel pools' addresses into voxelPoolInfo.
  void writeVoxelPoolInfo();
  // Copies counters into the frame's readback slot (0: main view, 1 + i:
  // cascade i).
  void readBackCounters(VkCommandBuffer cmd, const FrameData &frame, const Buffer &counters,
                        uint32_t slot);
  void readCounters(FrameData &frame);
  // VMA's allocated blocks on the device-local heaps (VRAM), and on all
  // heaps (system memory too).
  struct MemoryUse {
    VkDeviceSize deviceLocal = 0;
    VkDeviceSize all = 0;
  };
  MemoryUse memoryUse() const;
  // The largest memoryUse while models load (a growing pool's old and new
  // buffers coexist), printed with the TLAS line.
  MemoryUse loadPeak;
  // A cascade's cull: its own scene copy and counters (null: the main view).
  struct ShadowCull {
    VkDeviceAddress scene;
    const Buffer *counters;
  };
  CullConstants cullConstants(const FrameData &frame, uint32_t phase,
                              const ShadowCull *shadow = nullptr) const;
  void cullCells(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase,
                 const ShadowCull *shadow = nullptr);
  void cullInstances(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase,
                     const ShadowCull *shadow = nullptr);
  void traverseClusters(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase,
                        const ShadowCull *shadow = nullptr);
  void cullClusters(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase,
                    const ShadowCull *shadow = nullptr);
  void expandVoxelBricks(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase,
                         const ShadowCull *shadow = nullptr);
  // Raster sun shadows (renderer_shadows.cpp). plan runs on the CPU before
  // the scene upload and fills scene.sunShadows; record renders what plan chose.
  void createSunShadows();
  void destroySunShadows();
  void planSunShadows(SceneData &scene, uint32_t frameSlot, Vec3 camera, Vec3 sunDirection);
  void recordSunShadows(VkCommandBuffer cmd, const FrameData &frame);
  // Debug views and the pixel pick (renderer_debug.cpp). Everything here but
  // the CPU tables exists only while debugTargetsWanted().
  DebugView debugView = DebugView::None;
  bool debugTargetsKept = false;
  // The scene pipelines in use are the debug set (createScenePipelines).
  bool debugPipelines = false;
  Image debugId;
  // Scene attachment 3 when no diffuse albedo target exists (GI off, no RR).
  Image debugAlbedo;
  // Per global cluster index: its DAG level, 4 per uint (debug_view.frag).
  Buffer debugClusterLevels;
  // Two uvec4 per render pixel: the light split (debug_id.glsl
  // writeDebugLight), cleared to 0 before the scene pass.
  Buffer debugLight;
  DebugFilter debugFilter;
  VkDescriptorSetLayout debugSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool debugPool = VK_NULL_HANDLE;
  VkDescriptorSet debugSet = VK_NULL_HANDLE;
  VkPipelineLayout debugLayout = VK_NULL_HANDLE;
  VkPipeline debugPipeline = VK_NULL_HANDLE;
  // CPU only, filled by uploadMesh / setMeshName / setMaterialName.
  std::vector<uint8_t> clusterLevels;
  std::vector<uint32_t> clusterMeshes;
  std::vector<std::string> meshNames;
  std::vector<std::string> materialNames;
  bool debugTargetsWanted() const {
    return debugView != DebugView::None || debugTargetsKept ||
           debugFilter.mode != DebugFilterMode::None;
  }
  void updateDebugTargets();
  void createScenePipelines(bool debugId);
  void destroyScenePipelines();
  void destroyDebugTargets();
  // Inside a rendering pass onto a swapchain-format target of `extent`.
  void drawDebugView(VkCommandBuffer cmd, DebugView view, VkExtent2D extent);
  void selectTerrain(VkCommandBuffer cmd, const FrameData &frame);
  void drawTerrain(VkCommandBuffer cmd);
  void buildDepthPyramid(VkCommandBuffer cmd);
  void createDepthPyramid();

  void resetExposure();

  template <typename Record> void submitNow(Record &&record);
};

// IEEE half (GPU readbacks) to float.
float halfToFloat(uint16_t h);

// SceneData.debugFilter (debug_id.glsl): mode << 30 | material bit | target.
uint32_t encodeDebugFilter(const DebugFilter &filter);

// FNV-1a over bytes, for the Debug hashes of load-time GPU tables.
inline uint64_t fnv1a(uint64_t hash, const void *data, size_t bytes) {
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < bytes; ++i) {
    hash = (hash ^ p[i]) * 0x100000001b3ull;
  }
  return hash;
}

// Records commands with `record`, submits them and waits until the GPU is
// done. Only for loading, never per frame.
template <typename Record> void Renderer::Impl::submitNow(Record &&record) {
  VkDevice dev = device.device;
  check(vkResetFences(dev, 1, &uploadFence), "vkResetFences");
  check(vkResetCommandBuffer(uploadCommandBuffer, 0), "vkResetCommandBuffer");

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(uploadCommandBuffer, &beginInfo),
        "vkBeginCommandBuffer");
  record(uploadCommandBuffer);
  check(vkEndCommandBuffer(uploadCommandBuffer), "vkEndCommandBuffer");

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = uploadCommandBuffer;

  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;
  check(vkQueueSubmit2(graphicsQueue, 1, &submit, uploadFence),
        "vkQueueSubmit2");
  check(vkWaitForFences(dev, 1, &uploadFence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");
}

} // namespace meridian
