// Private state of meridian::Renderer, shared by renderer_setup.cpp,
// renderer_resources.cpp and renderer_frame.cpp.
#pragma once

#include "gpu_types.h"
#include "vulkan_util.h"

#include <meridian/renderer.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace meridian {

struct Renderer::Impl {
  GLFWwindow *window = nullptr;
  vkb::Instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  vkb::Device device;
  // Device functions from extensions (ray tracing), which the loader doesn't
  // export directly.
  vkb::DispatchTable dispatch;
  VmaAllocator allocator = nullptr;
  VkDeviceSize scratchAlignment = 0;

  VkQueue graphicsQueue = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily = 0;

  vkb::Swapchain swapchain;
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainImageViews;
  // One per swapchain image: the presentation engine may still be reading a
  // semaphore after our fence signals, so it can't be shared across images.
  std::vector<VkSemaphore> renderFinished;
  bool swapchainOutOfDate = false;

  // Sized like the swapchain and recreated with it.
  Image depth;
  Image hdr;
  Image motion;
  // Temporal anti-aliasing history, ping-ponged: each frame reads one and
  // writes the other. Always in VK_IMAGE_LAYOUT_GENERAL.
  std::array<Image, 2> history;
  // Min-reduced depth for occlusion culling: the screen rounded down to
  // powers of two, one view per level. Always in VK_IMAGE_LAYOUT_GENERAL.
  Image depthPyramid;
  std::vector<VkImageView> depthPyramidLevels;
  VkExtent2D depthPyramidExtent{};
  bool historyValid = false;
  Mat4 previousViewProjection;
  Mat4 previousView;
  // The depth pyramid holds last frame's depth at the current size, so the
  // early cluster cull can test against it.
  bool pyramidValid = false;

  std::array<FrameData, kFramesInFlight> frames;
  uint64_t frameNumber = 0;

  // Two GPU timestamps per frame in flight: start and end of the frame.
  VkQueryPool timestampPool = VK_NULL_HANDLE;
  float timestampPeriodNs = 0.0f;
  float gpuFrameMs = 0.0f;
  uint32_t drawnInstances = 0;
  uint32_t drawnClusters = 0;
  uint32_t drawnTriangles = 0;
  uint32_t droppedEntries = 0;
  float lodErrorPixels = 1.0f;
  bool occlusionCulling = true;

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
  VkPipeline cullPipeline = VK_NULL_HANDLE;
  VkPipeline clusterCullPipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout cullSetLayout = VK_NULL_HANDLE;
  // The depth pyramid, sampled through minSampler.
  VkDescriptorSet cullSet = VK_NULL_HANDLE;
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
  bool instancesDirty = false;
  // Cluster hierarchies of every mesh, bump-allocated like the pools above.
  Buffer clusterPool;
  Buffer clusterVertexPool;
  Buffer clusterTrianglePool;
  uint32_t clusterCount = 0;
  uint32_t clusterVertexCount = 0;
  uint32_t clusterTriangleCount = 0;
  // Culling output, both phases: work items for the cluster cull, cluster
  // draw lists, and the DrawCounters (indirect arguments and statistics).
  Buffer workItems;
  Buffer drawLists;
  // Clusters the early cull found occluded, retested by the late cull.
  Buffer retestClusters;
  Buffer drawCounters;
  // Per instance: visible at the end of last frame's culling (1) or not.
  Buffer visibility;

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
  // postSets[i] reads history[i] and writes history[1 - i].
  std::array<VkDescriptorSet, 2> postSets{};
  VkPipelineLayout postLayout = VK_NULL_HANDLE;
  VkPipeline taaPipeline = VK_NULL_HANDLE;
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
  std::chrono::steady_clock::time_point lastDrawTime;
  VkPipeline tonemapPipeline = VK_NULL_HANDLE;

  std::vector<GpuMesh> meshes;

  std::optional<std::filesystem::path> capturePath;

  void createSwapchain();
  void destroySwapchain();
  void createFrames();
  void createDescriptors();
  void createPipelines();
  void createExposureBuffers();
  void createScene();
  void createAtmosphere();
  void createMaterialSystem();
  void computeDfgLut();

  Image createImage(VkFormat format, VkExtent3D extent,
                    VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    uint32_t mipLevels = 1);
  Image createScreenImage(VkFormat format, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect);
  void destroyImage(Image &image);

  Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaAllocationCreateFlags flags,
                      VkDeviceSize alignment = 0);
  void destroyBuffer(Buffer &buffer);

  AccelerationStructure
  createAccelerationStructure(VkAccelerationStructureTypeKHR type,
                              VkDeviceSize size);
  void destroyAccelerationStructure(AccelerationStructure &as);
  void buildTlas(VkCommandBuffer cmd, FrameData &frame);
  void uploadInstances(VkCommandBuffer cmd, FrameData &frame);
  void cullInstances(VkCommandBuffer cmd, const FrameData &frame, uint32_t phase);
  void buildDepthPyramid(VkCommandBuffer cmd);
  void createDepthPyramid();

  template <typename Record> void submitNow(Record &&record);
};

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
