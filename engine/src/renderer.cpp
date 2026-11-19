#include <meridian/renderer.h>
#include <meridian/window.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define MERIDIAN_HAS_LSAN 1
#endif
#endif

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace meridian {

namespace {

// Two frames in flight: the CPU records frame N+1 while the GPU renders N.
constexpr uint32_t kFramesInFlight = 2;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// 16-bit float keeps smooth gradients across the full HDR range.
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Screen-space motion vectors for temporal anti-aliasing (and later DLSS).
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
// Halton(2,3) jitter cycle length.
constexpr uint32_t kJitterPhases = 8;
// Upper bound on draw items per frame, sizing each frame's TLAS.
constexpr uint32_t kMaxInstances = 4096;

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

struct GpuMesh {
  Buffer vertices;
  Buffer indices;
  uint32_t indexCount = 0;
  // Bottom-level acceleration structure: the mesh's triangles for ray tracing.
  AccelerationStructure blas;
};

// Must match SceneBuffer in the shaders (std430: each vec3 takes 16 bytes).
struct SceneData {
  Mat4 viewProjection;
  Mat4 currentViewProjection;
  Mat4 previousViewProjection;
  Vec3 sunDirection;
  float exposure = 1.0f;
  Vec3 sunIlluminance;
  uint32_t frameIndex = 0;
  Vec3 skyLuminance;
  float pad0 = 0.0f;
  VkDeviceAddress tlas = 0;
  uint64_t pad1 = 0;
};
static_assert(sizeof(SceneData) == 256);

// Must match the push_constant block in post.glsl.
struct PostConstants {
  uint32_t historyValid = 0;
};

// Must match the push_constant block in the shaders.
struct PushConstants {
  Mat4 model;
  VkDeviceAddress vertexBuffer = 0;
  VkDeviceAddress sceneBuffer = 0;
};
static_assert(sizeof(PushConstants) == 80);

struct FrameData {
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence renderFence = VK_NULL_HANDLE;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
  Buffer sceneBuffer;
  // Top-level acceleration structure over this frame's draw items, rebuilt
  // every frame from the instance buffer.
  Buffer instanceBuffer;
  AccelerationStructure tlas;
  Buffer tlasScratch;
};

// The NVIDIA driver leaks a few KB during device creation and on the first
// acceleration structure build in each command buffer. LeakSanitizer ignores
// allocations made while one of these lives; elsewhere it's a no-op.
#ifdef MERIDIAN_HAS_LSAN
using DriverLeakScope = __lsan::ScopedDisabler;
#else
struct DriverLeakScope {};
#endif

// Low-discrepancy sequence used for sub-pixel jitter; returns [0, 1).
float halton(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float fraction = 1.0f;
  while (index > 0) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

void check(VkResult result, const char *what) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(what) + " failed");
  }
}

struct ImageBarrier {
  VkImage image;
  VkImageAspectFlags aspect;
  VkImageLayout from;
  VkImageLayout to;
  VkPipelineStageFlags2 srcStage;
  VkAccessFlags2 srcAccess;
  VkPipelineStageFlags2 dstStage;
  VkAccessFlags2 dstAccess;
};

void transitionImage(VkCommandBuffer cmd, const ImageBarrier &b) {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = b.srcStage;
  barrier.srcAccessMask = b.srcAccess;
  barrier.dstStageMask = b.dstStage;
  barrier.dstAccessMask = b.dstAccess;
  barrier.oldLayout = b.from;
  barrier.newLayout = b.to;
  barrier.image = b.image;
  barrier.subresourceRange.aspectMask = b.aspect;
  barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

VkShaderModule loadShader(VkDevice device, const std::string &name) {
  std::string path = std::string(MERIDIAN_SHADER_DIR) + "/" + name + ".spv";
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("cannot open shader " + path);
  }

  std::vector<uint32_t> code(static_cast<size_t>(file.tellg()) /
                             sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(code.data()),
            static_cast<std::streamsize>(code.size() * sizeof(uint32_t)));

  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = code.size() * sizeof(uint32_t);
  info.pCode = code.data();

  VkShaderModule module = VK_NULL_HANDLE;
  check(vkCreateShaderModule(device, &info, nullptr, &module),
        "vkCreateShaderModule");
  return module;
}

struct PipelineDesc {
  const char *vertexShader = nullptr;
  const char *fragmentShader = nullptr;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::span<const VkFormat> colorFormats;
  VkFormat depthFormat = VK_FORMAT_UNDEFINED;
  VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
};

VkPipeline createGraphicsPipeline(VkDevice device, const PipelineDesc &desc) {
  VkShaderModule vertModule = loadShader(device, desc.vertexShader);
  VkShaderModule fragModule = loadShader(device, desc.fragmentShader);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName = "main";

  // Vertices are read in the shader through a buffer address ("vertex
  // pulling"), so the fixed-function vertex input stays empty.
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.cullMode = desc.cullMode;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Reverse-Z: nearer surfaces have larger depth values.
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = desc.depthFormat != VK_FORMAT_UNDEFINED;
  depthStencil.depthWriteEnable = desc.depthFormat != VK_FORMAT_UNDEFINED;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
      desc.colorFormats.size(), blendAttachment);

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
  colorBlend.pAttachments = blendAttachments.data();

  // Viewport and scissor are set per frame, so a resize doesn't need a new
  // pipeline.
  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                    VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamicStates;

  VkPipelineRenderingCreateInfo renderingInfo{};
  renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingInfo.colorAttachmentCount =
      static_cast<uint32_t>(desc.colorFormats.size());
  renderingInfo.pColorAttachmentFormats = desc.colorFormats.data();
  renderingInfo.depthAttachmentFormat = desc.depthFormat;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.pNext = &renderingInfo;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisample;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlend;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = desc.layout;

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                              &pipelineInfo, nullptr, &pipeline);
  vkDestroyShaderModule(device, vertModule, nullptr);
  vkDestroyShaderModule(device, fragModule, nullptr);
  check(result, "vkCreateGraphicsPipelines");
  return pipeline;
}

VkPipeline createComputePipeline(VkDevice device, const char *shader,
                                 VkPipelineLayout layout) {
  VkShaderModule module = loadShader(device, shader);

  VkComputePipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = layout;

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info,
                                             nullptr, &pipeline);
  vkDestroyShaderModule(device, module, nullptr);
  check(result, "vkCreateComputePipelines");
  return pipeline;
}

} // namespace

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
  bool historyValid = false;
  Mat4 previousViewProjection;

  std::array<FrameData, kFramesInFlight> frames;
  uint64_t frameNumber = 0;

  // Two GPU timestamps per frame in flight: start and end of the frame.
  VkQueryPool timestampPool = VK_NULL_HANDLE;
  float timestampPeriodNs = 0.0f;
  float gpuFrameMs = 0.0f;

  VkCommandPool uploadPool = VK_NULL_HANDLE;
  VkCommandBuffer uploadCommandBuffer = VK_NULL_HANDLE;
  VkFence uploadFence = VK_NULL_HANDLE;

  VkPipelineLayout meshLayout = VK_NULL_HANDLE;
  VkPipeline meshPipeline = VK_NULL_HANDLE;

  VkSampler nearestSampler = VK_NULL_HANDLE;
  VkSampler linearSampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  // postSets[i] reads history[i] and writes history[1 - i].
  std::array<VkDescriptorSet, 2> postSets{};
  VkPipelineLayout postLayout = VK_NULL_HANDLE;
  VkPipeline taaPipeline = VK_NULL_HANDLE;
  VkPipeline tonemapPipeline = VK_NULL_HANDLE;

  std::vector<GpuMesh> meshes;

  void createSwapchain();
  void destroySwapchain();
  void createFrames();
  void createDescriptors();
  void createPipelines();

  Image createImage(VkFormat format, VkImageUsageFlags usage,
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
  void buildTlas(VkCommandBuffer cmd, FrameData &frame,
                 std::span<const DrawItem> items);

  template <typename Record> void submitNow(Record &&record);
};

Buffer Renderer::Impl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VmaAllocationCreateFlags flags,
                                    VkDeviceSize alignment) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  allocInfo.flags = flags;

  Buffer buffer;
  VmaAllocationInfo info{};
  check(vmaCreateBufferWithAlignment(allocator, &bufferInfo, &allocInfo,
                                     alignment, &buffer.buffer,
                                     &buffer.allocation, &info),
        "vmaCreateBufferWithAlignment");
  buffer.mapped = info.pMappedData;

  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(device.device, &addressInfo);
  }
  return buffer;
}

void Renderer::Impl::destroyBuffer(Buffer &buffer) {
  vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
  buffer = {};
}

AccelerationStructure
Renderer::Impl::createAccelerationStructure(VkAccelerationStructureTypeKHR type,
                                            VkDeviceSize size) {
  AccelerationStructure as;
  as.buffer = createBuffer(size,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           0);

  VkAccelerationStructureCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  info.buffer = as.buffer.buffer;
  info.size = size;
  info.type = type;
  check(dispatch.createAccelerationStructureKHR(&info, nullptr, &as.handle),
        "vkCreateAccelerationStructureKHR");

  VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
  addressInfo.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
  addressInfo.accelerationStructure = as.handle;
  as.address = dispatch.getAccelerationStructureDeviceAddressKHR(&addressInfo);
  return as;
}

void Renderer::Impl::destroyAccelerationStructure(AccelerationStructure &as) {
  dispatch.destroyAccelerationStructureKHR(as.handle, nullptr);
  destroyBuffer(as.buffer);
  as = {};
}

void Renderer::Impl::buildTlas(VkCommandBuffer cmd, FrameData &frame,
                               std::span<const DrawItem> items) {
  if (items.size() > kMaxInstances) {
    throw std::runtime_error("too many draw items for the TLAS");
  }

  auto *instances = static_cast<VkAccelerationStructureInstanceKHR *>(
      frame.instanceBuffer.mapped);
  for (size_t i = 0; i < items.size(); ++i) {
    const Mat4 &m = items[i].transform;
    VkAccelerationStructureInstanceKHR instance{};
    // Vulkan wants a row-major 3x4 matrix; Mat4 is column-major.
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 4; ++column) {
        instance.transform.matrix[row][column] = m.m[column][row];
      }
    }
    instance.mask = 0xFF;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference =
        meshes[items[i].mesh.index].blas.address;
    instances[i] = instance;
  }
  check(vmaFlushAllocation(allocator, frame.instanceBuffer.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.geometry.instances.data.deviceAddress = frame.instanceBuffer.address;

  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.dstAccelerationStructure = frame.tlas.handle;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  build.scratchData.deviceAddress = frame.tlasScratch.address;

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = static_cast<uint32_t>(items.size());
  const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
  {
    DriverLeakScope driverLeak;
    dispatch.cmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
  }

  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

// A GPU-only 2D image the size of the swapchain.
Image Renderer::Impl::createImage(VkFormat format, VkImageUsageFlags usage,
                                  VkImageAspectFlags aspect) {
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {swapchain.extent.width, swapchain.extent.height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  // Render targets get their own allocation, which drivers handle best.
  allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

  Image image;
  check(vmaCreateImage(allocator, &imageInfo, &allocInfo, &image.image,
                       &image.allocation, nullptr),
        "vmaCreateImage");

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = image.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.subresourceRange.aspectMask = aspect;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  check(vkCreateImageView(device.device, &viewInfo, nullptr, &image.view),
        "vkCreateImageView");
  return image;
}

void Renderer::Impl::destroyImage(Image &image) {
  vkDestroyImageView(device.device, image.view, nullptr);
  vmaDestroyImage(allocator, image.image, image.allocation);
  image = {};
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

void Renderer::Impl::createSwapchain() {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);

  auto built =
      vkb::SwapchainBuilder{device}
          .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB,
                               VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
          .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
          .set_desired_extent(width, height)
          .build();
  if (!built) {
    throw std::runtime_error(built.error().message());
  }
  swapchain = built.value();
  swapchainImages = swapchain.get_images().value();
  swapchainImageViews = swapchain.get_image_views().value();

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  renderFinished.resize(swapchainImages.size());
  for (VkSemaphore &semaphore : renderFinished) {
    check(vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &semaphore),
          "vkCreateSemaphore");
  }

  depth = createImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);
  hdr = createImage(kHdrFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
  motion = createImage(kMotionFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
  for (Image &image : history) {
    image = createImage(kHdrFormat,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
  }
  submitNow([&](VkCommandBuffer cmd) {
    for (Image &image : history) {
      transitionImage(cmd, {image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }
  });
  historyValid = false;

  // Point the post-processing sets at the new images. Safe because the
  // device is idle whenever the swapchain is (re)created.
  for (int i = 0; i < 2; ++i) {
    const Image &previous = history[i];
    const Image &next = history[1 - i];
    VkDescriptorImageInfo images[] = {
        {nearestSampler, hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearestSampler, motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {linearSampler, previous.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, next.view, VK_IMAGE_LAYOUT_GENERAL},
        {nearestSampler, next.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = postSets[i];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = b == 3
                                     ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(device.device, 5, writes, 0, nullptr);
  }

  swapchainOutOfDate = false;

  std::cout << "Swapchain: " << swapchainImages.size() << " images, "
            << swapchain.extent.width << "x" << swapchain.extent.height
            << "\n";
}

void Renderer::Impl::destroySwapchain() {
  for (Image &image : history) {
    destroyImage(image);
  }
  destroyImage(motion);
  destroyImage(hdr);
  destroyImage(depth);

  for (VkSemaphore semaphore : renderFinished) {
    vkDestroySemaphore(device.device, semaphore, nullptr);
  }
  renderFinished.clear();
  swapchain.destroy_image_views(swapchainImageViews);
  vkb::destroy_swapchain(swapchain);
}

void Renderer::Impl::createFrames() {
  VkDevice dev = device.device;

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = graphicsQueueFamily;

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  for (FrameData &frame : frames) {
    check(vkCreateCommandPool(dev, &poolInfo, nullptr, &frame.commandPool),
          "vkCreateCommandPool");
    allocInfo.commandPool = frame.commandPool;
    check(vkAllocateCommandBuffers(dev, &allocInfo, &frame.commandBuffer),
          "vkAllocateCommandBuffers");
    check(vkCreateFence(dev, &fenceInfo, nullptr, &frame.renderFence),
          "vkCreateFence");
    check(vkCreateSemaphore(dev, &semaphoreInfo, nullptr,
                            &frame.imageAvailable),
          "vkCreateSemaphore");
    frame.sceneBuffer = createBuffer(
        sizeof(SceneData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.instanceBuffer = createBuffer(
        kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }

  // Size each TLAS for the maximum instance count once, so per-frame
  // rebuilds never allocate.
  VkAccelerationStructureGeometryKHR tlasGeometry{};
  tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  tlasGeometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

  VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
  tlasBuild.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  tlasBuild.geometryCount = 1;
  tlasBuild.pGeometries = &tlasGeometry;

  VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
  tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  dispatch.getAccelerationStructureBuildSizesKHR(
      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
      &kMaxInstances, &tlasSizes);

  for (FrameData &frame : frames) {
    frame.tlas = createAccelerationStructure(
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        tlasSizes.accelerationStructureSize);
    frame.tlasScratch = createBuffer(
        tlasSizes.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        0, scratchAlignment);
  }

  check(vkCreateCommandPool(dev, &poolInfo, nullptr, &uploadPool),
        "vkCreateCommandPool");
  allocInfo.commandPool = uploadPool;
  check(vkAllocateCommandBuffers(dev, &allocInfo, &uploadCommandBuffer),
        "vkAllocateCommandBuffers");
  fenceInfo.flags = 0;
  check(vkCreateFence(dev, &fenceInfo, nullptr, &uploadFence),
        "vkCreateFence");

  VkQueryPoolCreateInfo queryInfo{};
  queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
  queryInfo.queryCount = kFramesInFlight * 2;
  check(vkCreateQueryPool(dev, &queryInfo, nullptr, &timestampPool),
        "vkCreateQueryPool");
}

void Renderer::Impl::createDescriptors() {
  VkDevice dev = device.device;

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &nearestSampler),
        "vkCreateSampler");
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &linearSampler),
        "vkCreateSampler");

  // Must match post.glsl.
  constexpr VkShaderStageFlags kPostStages =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutBinding bindings[5]{};
  for (uint32_t b = 0; b < 5; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorType = b == 3
                                     ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[b].descriptorCount = 1;
    bindings[b].stageFlags = kPostStages;
  }

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 5;
  layoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &postSetLayout),
        "vkCreateDescriptorSetLayout");

  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
  };
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 2;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool),
        "vkCreateDescriptorPool");

  VkDescriptorSetLayout setLayouts[] = {postSetLayout, postSetLayout};
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool;
  allocInfo.descriptorSetCount = 2;
  allocInfo.pSetLayouts = setLayouts;
  check(vkAllocateDescriptorSets(dev, &allocInfo, postSets.data()),
        "vkAllocateDescriptorSets");
}

void Renderer::Impl::createPipelines() {
  VkDevice dev = device.device;

  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushRange.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo meshLayoutInfo{};
  meshLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  meshLayoutInfo.pushConstantRangeCount = 1;
  meshLayoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &meshLayoutInfo, nullptr, &meshLayout),
        "vkCreatePipelineLayout");

  const VkFormat sceneFormats[] = {kHdrFormat, kMotionFormat};
  meshPipeline = createGraphicsPipeline(dev, {.vertexShader = "mesh.vert",
                                              .fragmentShader = "mesh.frag",
                                              .layout = meshLayout,
                                              .colorFormats = sceneFormats,
                                              .depthFormat = kDepthFormat,
                                              .cullMode = VK_CULL_MODE_BACK_BIT});

  VkPushConstantRange postPushRange{};
  postPushRange.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  postPushRange.size = sizeof(PostConstants);

  VkPipelineLayoutCreateInfo postLayoutInfo{};
  postLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  postLayoutInfo.setLayoutCount = 1;
  postLayoutInfo.pSetLayouts = &postSetLayout;
  postLayoutInfo.pushConstantRangeCount = 1;
  postLayoutInfo.pPushConstantRanges = &postPushRange;
  check(vkCreatePipelineLayout(dev, &postLayoutInfo, nullptr, &postLayout),
        "vkCreatePipelineLayout");

  taaPipeline = createComputePipeline(dev, "taa.comp", postLayout);

  const VkFormat swapchainFormat[] = {swapchain.image_format};
  tonemapPipeline =
      createGraphicsPipeline(dev, {.vertexShader = "fullscreen.vert",
                                   .fragmentShader = "tonemap.frag",
                                   .layout = postLayout,
                                   .colorFormats = swapchainFormat});
}

Renderer::Renderer(Window &window) : impl_(std::make_unique<Impl>()) {
  impl_->window = window.handle();

  vkb::InstanceBuilder instanceBuilder;
  instanceBuilder.set_app_name("Meridian").require_api_version(1, 3, 0);
  if (kEnableValidation) {
    instanceBuilder.request_validation_layers().use_default_debug_messenger();
  }
  auto instance = instanceBuilder.build();
  if (!instance) {
    throw std::runtime_error(instance.error().message());
  }
  impl_->instance = instance.value();

  check(glfwCreateWindowSurface(impl_->instance.instance, impl_->window,
                                nullptr, &impl_->surface),
        "glfwCreateWindowSurface");

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  features12.bufferDeviceAddress = VK_TRUE;

  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;

  // Hardware ray tracing is required: shadows (and later reflections and GI)
  // are traced with ray queries.
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{};
  accelerationFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
  accelerationFeatures.accelerationStructure = VK_TRUE;

  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
  rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
  rayQueryFeatures.rayQuery = VK_TRUE;

  auto physical =
      vkb::PhysicalDeviceSelector{impl_->instance}
          .set_minimum_version(1, 3)
          .set_required_features_12(features12)
          .set_required_features_13(features13)
          .add_required_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
          .add_required_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME)
          .add_required_extension(
              VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
          .add_required_extension_features(accelerationFeatures)
          .add_required_extension_features(rayQueryFeatures)
          .set_surface(impl_->surface)
          .select();
  if (!physical) {
    throw std::runtime_error(physical.error().message());
  }

  auto device = [&] {
    DriverLeakScope driverLeak;
    return vkb::DeviceBuilder{physical.value()}.build();
  }();
  if (!device) {
    throw std::runtime_error(device.error().message());
  }
  impl_->device = device.value();
  impl_->dispatch = impl_->device.make_table();

  VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{};
  accelerationProperties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &accelerationProperties;
  vkGetPhysicalDeviceProperties2(physical.value().physical_device, &properties);
  impl_->scratchAlignment =
      accelerationProperties.minAccelerationStructureScratchOffsetAlignment;

  impl_->graphicsQueue =
      impl_->device.get_queue(vkb::QueueType::graphics).value();
  impl_->graphicsQueueFamily =
      impl_->device.get_queue_index(vkb::QueueType::graphics).value();

  VmaAllocatorCreateInfo allocatorInfo{};
  allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  allocatorInfo.physicalDevice = physical.value().physical_device;
  allocatorInfo.device = impl_->device.device;
  allocatorInfo.instance = impl_->instance.instance;
  allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
  check(vmaCreateAllocator(&allocatorInfo, &impl_->allocator),
        "vmaCreateAllocator");

  impl_->timestampPeriodNs = physical.value().properties.limits.timestampPeriod;
  impl_->createFrames();
  impl_->createDescriptors();
  impl_->createSwapchain();
  impl_->createPipelines();

  std::cout << "GPU: " << physical.value().name << "\n";
}

MeshId Renderer::uploadMesh(const MeshData &data) {
  const VkDeviceSize vertexBytes = data.vertices.size() * sizeof(Vertex);
  const VkDeviceSize indexBytes = data.indices.size() * sizeof(uint32_t);

  GpuMesh mesh;
  mesh.indexCount = static_cast<uint32_t>(data.indices.size());
  constexpr VkBufferUsageFlags kRayTracingInput =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  mesh.vertices = impl_->createBuffer(
      vertexBytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          kRayTracingInput,
      0);
  mesh.indices = impl_->createBuffer(
      indexBytes,
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          kRayTracingInput,
      0);

  // GPU-only memory is fastest to read but the CPU can't write it, so the
  // data goes through a temporary CPU-visible staging buffer.
  Buffer staging = impl_->createBuffer(
      vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memcpy(staging.mapped, data.vertices.data(), vertexBytes);
  std::memcpy(static_cast<char *>(staging.mapped) + vertexBytes,
              data.indices.data(), indexBytes);
  check(vmaFlushAllocation(impl_->allocator, staging.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  auto &triangles = geometry.geometry.triangles;
  triangles.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  triangles.vertexData.deviceAddress = mesh.vertices.address;
  triangles.vertexStride = sizeof(Vertex);
  triangles.maxVertex = static_cast<uint32_t>(data.vertices.size() - 1);
  triangles.indexType = VK_INDEX_TYPE_UINT32;
  triangles.indexData.deviceAddress = mesh.indices.address;

  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;

  const uint32_t triangleCount = mesh.indexCount / 3;
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  impl_->dispatch.getAccelerationStructureBuildSizesKHR(
      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &triangleCount,
      &sizes);

  mesh.blas = impl_->createAccelerationStructure(
      VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      sizes.accelerationStructureSize);
  Buffer scratch = impl_->createBuffer(
      sizes.buildScratchSize,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0, impl_->scratchAlignment);
  build.dstAccelerationStructure = mesh.blas.handle;
  build.scratchData.deviceAddress = scratch.address;

  impl_->submitNow([&](VkCommandBuffer cmd) {
    VkBufferCopy vertexCopy{0, 0, vertexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, mesh.vertices.buffer, 1, &vertexCopy);
    VkBufferCopy indexCopy{vertexBytes, 0, indexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, mesh.indices.buffer, 1, &indexCopy);

    VkMemoryBarrier2 copied{};
    copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    copied.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    copied.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    copied.dstStageMask =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    copied.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_INDEX_READ_BIT |
                           VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &copied;
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = triangleCount;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    {
      DriverLeakScope driverLeak;
      impl_->dispatch.cmdBuildAccelerationStructuresKHR(cmd, 1, &build,
                                                        &ranges);
    }

    // Later TLAS builds reference this BLAS.
    VkMemoryBarrier2 built{};
    built.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    built.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    built.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    built.dstStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    built.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    dependency.pMemoryBarriers = &built;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });

  impl_->destroyBuffer(scratch);
  impl_->destroyBuffer(staging);

  impl_->meshes.push_back(mesh);
  return {static_cast<uint32_t>(impl_->meshes.size() - 1)};
}

float Renderer::gpuFrameMs() const { return impl_->gpuFrameMs; }

void Renderer::draw(const Camera &camera, const Lighting &lighting,
                    std::span<const DrawItem> items) {
  VkDevice dev = impl_->device.device;

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(impl_->window, &width, &height);
  if (width == 0 || height == 0) {
    return;
  }

  // Wayland never reports the swapchain as out of date on resize, so the
  // size has to be compared by hand.
  if (impl_->swapchainOutOfDate ||
      static_cast<uint32_t>(width) != impl_->swapchain.extent.width ||
      static_cast<uint32_t>(height) != impl_->swapchain.extent.height) {
    check(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");
    impl_->destroySwapchain();
    impl_->createSwapchain();
  }

  const uint32_t frameSlot =
      static_cast<uint32_t>(impl_->frameNumber % kFramesInFlight);
  FrameData &frame = impl_->frames[frameSlot];
  check(vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");

  // Once each slot has been submitted, the fence guarantees its previous
  // timestamps are written.
  uint64_t timestamps[2] = {};
  if (impl_->frameNumber >= kFramesInFlight &&
      vkGetQueryPoolResults(dev, impl_->timestampPool, frameSlot * 2, 2,
                            sizeof(timestamps), timestamps, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    impl_->gpuFrameMs = static_cast<float>(timestamps[1] - timestamps[0]) *
                        impl_->timestampPeriodNs * 1e-6f;
  }

  uint32_t imageIndex = 0;
  VkResult acquired = vkAcquireNextImageKHR(
      dev, impl_->swapchain.swapchain, UINT64_MAX, frame.imageAvailable,
      VK_NULL_HANDLE, &imageIndex);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    impl_->swapchainOutOfDate = true;
    return;
  }
  if (acquired != VK_SUBOPTIMAL_KHR) {
    check(acquired, "vkAcquireNextImageKHR");
  }

  // Reset only once we know we'll submit, or the next wait never returns.
  check(vkResetFences(dev, 1, &frame.renderFence), "vkResetFences");

  const VkExtent2D extent = impl_->swapchain.extent;
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);

  // Exposure is applied while shading ("pre-exposure") so the HDR target
  // holds display-range values: physical luminances like the sun disk would
  // overflow 16-bit floats.
  // Sub-pixel jitter in pixels, cycling through Halton(2,3). The temporal
  // resolve turns it into anti-aliasing (and DLSS will need it too).
  const uint32_t phase =
      static_cast<uint32_t>(impl_->frameNumber % kJitterPhases) + 1;
  const float jitterX = halton(phase, 2) - 0.5f;
  const float jitterY = halton(phase, 3) - 0.5f;

  const Mat4 view = camera.view();
  const Mat4 projection = camera.projection(aspect);
  Mat4 jitteredProjection = projection;
  jitteredProjection.m[2][0] += 2.0f * jitterX / static_cast<float>(extent.width);
  jitteredProjection.m[2][1] += 2.0f * jitterY / static_cast<float>(extent.height);

  SceneData scene;
  scene.viewProjection = jitteredProjection * view;
  scene.currentViewProjection = projection * view;
  scene.previousViewProjection = impl_->historyValid
                                     ? impl_->previousViewProjection
                                     : scene.currentViewProjection;
  scene.frameIndex = static_cast<uint32_t>(impl_->frameNumber);
  impl_->previousViewProjection = scene.currentViewProjection;
  scene.sunDirection = lighting.sunDirection;
  scene.exposure = camera.exposure();
  scene.sunIlluminance = lighting.sunIlluminance;
  scene.skyLuminance = lighting.skyLuminance;
  scene.tlas = frame.tlas.address;
  std::memcpy(frame.sceneBuffer.mapped, &scene, sizeof(scene));
  check(vmaFlushAllocation(impl_->allocator, frame.sceneBuffer.allocation, 0,
                           VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  VkCommandBuffer cmd = frame.commandBuffer;
  check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer");

  vkCmdResetQueryPool(cmd, impl_->timestampPool, frameSlot * 2, 2);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_NONE, impl_->timestampPool,
                       frameSlot * 2);

  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  VkRect2D scissor = {{0, 0}, extent};

  impl_->buildTlas(cmd, frame, items);

  // --- Scene pass: shade into the HDR target.

  // The HDR and depth images are shared by both frames in flight, so each
  // barrier also waits for the previous frame's use of the image.
  for (VkImage target : {impl_->hdr.image, impl_->motion.image}) {
    transitionImage(cmd, {target, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
  }

  constexpr VkPipelineStageFlags2 kDepthStages =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  transitionImage(cmd, {impl_->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthStages,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        kDepthStages,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT});

  const Vec3 sky = lighting.skyLuminance * scene.exposure;

  VkRenderingAttachmentInfo hdrAttachment{};
  hdrAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  hdrAttachment.imageView = impl_->hdr.view;
  hdrAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  hdrAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  hdrAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  hdrAttachment.clearValue.color = {{sky.x, sky.y, sky.z, 1.0f}};

  VkRenderingAttachmentInfo depthAttachment{};
  depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depthAttachment.imageView = impl_->depth.view;
  depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.clearValue.depthStencil.depth = 0.0f;

  VkRenderingInfo sceneRendering{};
  sceneRendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  sceneRendering.renderArea = {{0, 0}, extent};
  sceneRendering.layerCount = 1;
  VkRenderingAttachmentInfo motionAttachment{};
  motionAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  motionAttachment.imageView = impl_->motion.view;
  motionAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  motionAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  motionAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  const VkRenderingAttachmentInfo sceneAttachments[] = {hdrAttachment,
                                                        motionAttachment};
  sceneRendering.colorAttachmentCount = 2;
  sceneRendering.pColorAttachments = sceneAttachments;
  sceneRendering.pDepthAttachment = &depthAttachment;

  vkCmdBeginRendering(cmd, &sceneRendering);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    impl_->meshPipeline);
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  for (const DrawItem &item : items) {
    const GpuMesh &mesh = impl_->meshes[item.mesh.index];

    PushConstants push;
    push.model = item.transform;
    push.vertexBuffer = mesh.vertices.address;
    push.sceneBuffer = frame.sceneBuffer.address;
    vkCmdPushConstants(cmd, impl_->meshLayout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
  }

  vkCmdEndRendering(cmd);

  // --- Temporal resolve: scene color + history -> new history.

  for (VkImage target : {impl_->hdr.image, impl_->motion.image}) {
    transitionImage(cmd, {target, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  }

  // The history images stay in GENERAL. This frame reads what the previous
  // frame wrote, and overwrites the image the previous frame's tonemap read.
  VkMemoryBarrier2 historyBarrier{};
  historyBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  historyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  historyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  historyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  historyBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo historyDependency{};
  historyDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  historyDependency.memoryBarrierCount = 1;
  historyDependency.pMemoryBarriers = &historyBarrier;
  vkCmdPipelineBarrier2(cmd, &historyDependency);

  VkDescriptorSet postSet = impl_->postSets[impl_->frameNumber % 2];
  PostConstants postConstants;
  postConstants.historyValid = impl_->historyValid ? 1 : 0;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->taaPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->postLayout, 0, 1, &postSet, 0, nullptr);
  vkCmdPushConstants(cmd, impl_->postLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(postConstants), &postConstants);
  vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);

  historyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  historyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  historyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  historyBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  vkCmdPipelineBarrier2(cmd, &historyDependency);
  impl_->historyValid = true;

  // --- Tonemap pass: resolved HDR -> swapchain image.

  VkImage swapchainImage = impl_->swapchainImages[imageIndex];
  transitionImage(cmd, {swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

  VkRenderingAttachmentInfo swapchainAttachment{};
  swapchainAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  swapchainAttachment.imageView = impl_->swapchainImageViews[imageIndex];
  swapchainAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swapchainAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo tonemapRendering{};
  tonemapRendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  tonemapRendering.renderArea = {{0, 0}, extent};
  tonemapRendering.layerCount = 1;
  tonemapRendering.colorAttachmentCount = 1;
  tonemapRendering.pColorAttachments = &swapchainAttachment;

  vkCmdBeginRendering(cmd, &tonemapRendering);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    impl_->tonemapPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          impl_->postLayout, 0, 1, &postSet, 0, nullptr);
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  transitionImage(cmd, {swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE, 0});

  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       impl_->timestampPool, frameSlot * 2 + 1);

  check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;

  VkSemaphoreSubmitInfo waitInfo{};
  waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  waitInfo.semaphore = frame.imageAvailable;
  waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkSemaphoreSubmitInfo signalInfo{};
  signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signalInfo.semaphore = impl_->renderFinished[imageIndex];
  signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &waitInfo;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signalInfo;
  check(vkQueueSubmit2(impl_->graphicsQueue, 1, &submit, frame.renderFence),
        "vkQueueSubmit2");

  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &impl_->renderFinished[imageIndex];
  present.swapchainCount = 1;
  present.pSwapchains = &impl_->swapchain.swapchain;
  present.pImageIndices = &imageIndex;
  VkResult presented = vkQueuePresentKHR(impl_->graphicsQueue, &present);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR ||
      presented == VK_SUBOPTIMAL_KHR) {
    impl_->swapchainOutOfDate = true;
  } else {
    check(presented, "vkQueuePresentKHR");
  }

  ++impl_->frameNumber;
}

Renderer::~Renderer() {
  VkDevice dev = impl_->device.device;
  vkDeviceWaitIdle(dev);

  for (GpuMesh &mesh : impl_->meshes) {
    impl_->destroyAccelerationStructure(mesh.blas);
    impl_->destroyBuffer(mesh.vertices);
    impl_->destroyBuffer(mesh.indices);
  }

  vkDestroyPipeline(dev, impl_->tonemapPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->taaPipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->postLayout, nullptr);
  vkDestroyPipeline(dev, impl_->meshPipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->meshLayout, nullptr);

  vkDestroyQueryPool(dev, impl_->timestampPool, nullptr);
  vkDestroyFence(dev, impl_->uploadFence, nullptr);
  vkDestroyCommandPool(dev, impl_->uploadPool, nullptr);
  for (FrameData &frame : impl_->frames) {
    impl_->destroyAccelerationStructure(frame.tlas);
    impl_->destroyBuffer(frame.tlasScratch);
    impl_->destroyBuffer(frame.instanceBuffer);
    impl_->destroyBuffer(frame.sceneBuffer);
    vkDestroySemaphore(dev, frame.imageAvailable, nullptr);
    vkDestroyFence(dev, frame.renderFence, nullptr);
    vkDestroyCommandPool(dev, frame.commandPool, nullptr);
  }

  impl_->destroySwapchain();
  vkDestroyDescriptorPool(dev, impl_->descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->postSetLayout, nullptr);
  vkDestroySampler(dev, impl_->linearSampler, nullptr);
  vkDestroySampler(dev, impl_->nearestSampler, nullptr);
  vmaDestroyAllocator(impl_->allocator);

  vkb::destroy_device(impl_->device);
  vkb::destroy_surface(impl_->instance, impl_->surface);
  vkb::destroy_instance(impl_->instance);
}

} // namespace meridian
