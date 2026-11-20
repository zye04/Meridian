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
#include <vector>

namespace meridian {

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
  uint32_t droppedEntries = 0;
  std::array<uint32_t, 5> droppedByList{};
  uint32_t drawnTerrainNodes = 0;
  uint32_t drawnCells = 0;
  uint32_t drawnFarCells = 0;
  uint32_t visitedBvhNodes = 0;
  float lodErrorPixels = 1.0f;
  bool occlusionCulling = true;
  bool voxelFarShadows = true;

  VkCommandPool uploadPool = VK_NULL_HANDLE;
  VkCommandBuffer uploadCommandBuffer = VK_NULL_HANDLE;
  VkFence uploadFence = VK_NULL_HANDLE;

  // Shared by everything that reads the scene buffer: meshes, the sky and
  // the atmosphere compute passes.
  VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
  VkPipeline meshPipeline = VK_NULL_HANDLE;
  VkPipeline maskedMeshPipeline = VK_NULL_HANDLE;
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
  // kMaxInstances records.
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
  VkPipeline voxelPipeline = VK_NULL_HANDLE;

  VkSampler nearestSampler = VK_NULL_HANDLE;
  VkSampler linearSampler = VK_NULL_HANDLE;
  // Linear filtering that returns the minimum of the footprint.
  VkSampler minSampler = VK_NULL_HANDLE;
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
  VkPipeline cloudSlabPipeline = VK_NULL_HANDLE;
  VkPipeline skyCapturePipeline = VK_NULL_HANDLE;
  CloudRenderer cloudRenderer = CloudRenderer::Volumetric;
  // The resolution last frame's history was resolved at (none yet: 2).
  uint32_t cloudHistoryResolution = 2;
  // Bottom and top (m) of the cloud layer at the camera, from setWeather.
  float cameraCloudLayer[2] = {};
  // Last frame's cloud wind offset modulo a short period, for the
  // displacement since then.
  float cloudWindLocal[2] = {};
  bool cloudWindLocalValid = false;

  std::vector<GpuMesh> meshes;

  std::optional<std::filesystem::path> capturePath;

  void createSwapchain();
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
  void destroyClouds();
  void placeWeatherMap(Vec3 cameraPosition);
  void drawWeatherMap(VkCommandBuffer cmd, const FrameData &frame);
  void placeCloudShadow(Vec3 cameraPosition, Vec3 sunDirection);
  void drawCloudShadow(VkCommandBuffer cmd, const FrameData &frame);
  void drawSkyCapture(VkCommandBuffer cmd, const FrameData &frame);
  void createCloudTargets();
  void destroyCloudTargets();
  bool cameraInCloudLayer(Vec3 cameraPosition) const;
  void drawClouds(VkCommandBuffer cmd, const FrameData &frame, Vec3 cameraPosition);
  void drawCloudsSlab(VkCommandBuffer cmd, const FrameData &frame);

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
  // The far field of the current placement: prototype bake, then the
  // splat over the placed table (offsets: setPlacement's per (cell, layer)).
  void buildFarField(std::span<const Prototype> prototypes,
                     std::span<const PlacementLayer> layers,
                     const std::vector<uint32_t> &offsets);
  void destroyFarField();
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
  void resetDrawCounters(VkCommandBuffer cmd);
  CullConstants cullConstants(const FrameData &frame, uint32_t phase) const;
  void cullCells(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase);
  void cullInstances(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase);
  void traverseClusters(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase);
  void cullClusters(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase);
  void selectTerrain(VkCommandBuffer cmd, const FrameData &frame);
  void drawTerrain(VkCommandBuffer cmd);
  void buildDepthPyramid(VkCommandBuffer cmd);
  void createDepthPyramid();

  template <typename Record> void submitNow(Record &&record);
};

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
