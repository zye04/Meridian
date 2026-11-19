#include <meridian/renderer.h>
#include <meridian/window.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

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

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkDeviceAddress address = 0;
  void *mapped = nullptr;
};

struct GpuMesh {
  Buffer vertices;
  Buffer indices;
  uint32_t indexCount = 0;
};

// Must match SceneBuffer in the shaders (std430: each vec3 takes 16 bytes).
struct SceneData {
  Mat4 viewProjection;
  Vec3 sunDirection;
  float pad0 = 0.0f;
  Vec3 sunColor;
  float pad1 = 0.0f;
  Vec3 skyColor;
  float pad2 = 0.0f;
};
static_assert(sizeof(SceneData) == 112);

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
};

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

} // namespace

struct Renderer::Impl {
  GLFWwindow *window = nullptr;
  vkb::Instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  vkb::Device device;
  VmaAllocator allocator = nullptr;

  VkQueue graphicsQueue = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily = 0;

  vkb::Swapchain swapchain;
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainImageViews;
  // One per swapchain image: the presentation engine may still be reading a
  // semaphore after our fence signals, so it can't be shared across images.
  std::vector<VkSemaphore> renderFinished;
  bool swapchainOutOfDate = false;

  VkImage depthImage = VK_NULL_HANDLE;
  VmaAllocation depthAllocation = nullptr;
  VkImageView depthView = VK_NULL_HANDLE;

  std::array<FrameData, kFramesInFlight> frames;
  uint64_t frameNumber = 0;

  VkCommandPool uploadPool = VK_NULL_HANDLE;
  VkCommandBuffer uploadCommandBuffer = VK_NULL_HANDLE;
  VkFence uploadFence = VK_NULL_HANDLE;

  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;

  std::vector<GpuMesh> meshes;

  void createSwapchain();
  void destroySwapchain();
  void createFrames();
  void createPipeline();

  Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaAllocationCreateFlags flags);
  void destroyBuffer(Buffer &buffer);

  template <typename Record> void submitNow(Record &&record);
};

Buffer Renderer::Impl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VmaAllocationCreateFlags flags) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  allocInfo.flags = flags;

  Buffer buffer;
  VmaAllocationInfo info{};
  check(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer,
                        &buffer.allocation, &info),
        "vmaCreateBuffer");
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

  VkImageCreateInfo depthInfo{};
  depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  depthInfo.imageType = VK_IMAGE_TYPE_2D;
  depthInfo.format = kDepthFormat;
  depthInfo.extent = {swapchain.extent.width, swapchain.extent.height, 1};
  depthInfo.mipLevels = 1;
  depthInfo.arrayLayers = 1;
  depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

  VmaAllocationCreateInfo depthAlloc{};
  depthAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  check(vmaCreateImage(allocator, &depthInfo, &depthAlloc, &depthImage,
                       &depthAllocation, nullptr),
        "vmaCreateImage");

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = depthImage;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = kDepthFormat;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  check(vkCreateImageView(device.device, &viewInfo, nullptr, &depthView),
        "vkCreateImageView");

  swapchainOutOfDate = false;

  std::cout << "Swapchain: " << swapchainImages.size() << " images, "
            << swapchain.extent.width << "x" << swapchain.extent.height
            << "\n";
}

void Renderer::Impl::destroySwapchain() {
  vkDestroyImageView(device.device, depthView, nullptr);
  vmaDestroyImage(allocator, depthImage, depthAllocation);

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
  }

  check(vkCreateCommandPool(dev, &poolInfo, nullptr, &uploadPool),
        "vkCreateCommandPool");
  allocInfo.commandPool = uploadPool;
  check(vkAllocateCommandBuffers(dev, &allocInfo, &uploadCommandBuffer),
        "vkAllocateCommandBuffers");
  fenceInfo.flags = 0;
  check(vkCreateFence(dev, &fenceInfo, nullptr, &uploadFence),
        "vkCreateFence");
}

void Renderer::Impl::createPipeline() {
  VkDevice dev = device.device;

  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushRange.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout),
        "vkCreatePipelineLayout");

  VkShaderModule vertModule = loadShader(dev, "mesh.vert");
  VkShaderModule fragModule = loadShader(dev, "mesh.frag");

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
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Reverse-Z: nearer surfaces have larger depth values.
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  // Viewport and scissor are set per frame, so a resize doesn't need a new
  // pipeline.
  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                    VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamicStates;

  VkFormat colorFormat = swapchain.image_format;
  VkPipelineRenderingCreateInfo renderingInfo{};
  renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachmentFormats = &colorFormat;
  renderingInfo.depthAttachmentFormat = kDepthFormat;

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
  pipelineInfo.layout = pipelineLayout;

  VkResult result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1,
                                              &pipelineInfo, nullptr, &pipeline);
  vkDestroyShaderModule(dev, vertModule, nullptr);
  vkDestroyShaderModule(dev, fragModule, nullptr);
  check(result, "vkCreateGraphicsPipelines");
}

Renderer::Renderer(Window &window) : impl_(std::make_unique<Impl>()) {
  impl_->window = window.handle();

  auto instance = vkb::InstanceBuilder{}
                      .set_app_name("Meridian")
                      .request_validation_layers()
                      .use_default_debug_messenger()
                      .require_api_version(1, 3, 0)
                      .build();
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

  auto physical = vkb::PhysicalDeviceSelector{impl_->instance}
                      .set_minimum_version(1, 3)
                      .set_required_features_12(features12)
                      .set_required_features_13(features13)
                      .set_surface(impl_->surface)
                      .select();
  if (!physical) {
    throw std::runtime_error(physical.error().message());
  }

  auto device = vkb::DeviceBuilder{physical.value()}.build();
  if (!device) {
    throw std::runtime_error(device.error().message());
  }
  impl_->device = device.value();

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

  impl_->createSwapchain();
  impl_->createFrames();
  impl_->createPipeline();

  std::cout << "GPU: " << physical.value().name << "\n";
}

MeshId Renderer::uploadMesh(const MeshData &data) {
  const VkDeviceSize vertexBytes = data.vertices.size() * sizeof(Vertex);
  const VkDeviceSize indexBytes = data.indices.size() * sizeof(uint32_t);

  GpuMesh mesh;
  mesh.indexCount = static_cast<uint32_t>(data.indices.size());
  mesh.vertices = impl_->createBuffer(
      vertexBytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);
  mesh.indices = impl_->createBuffer(
      indexBytes,
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);

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

  impl_->submitNow([&](VkCommandBuffer cmd) {
    VkBufferCopy vertexCopy{0, 0, vertexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, mesh.vertices.buffer, 1, &vertexCopy);
    VkBufferCopy indexCopy{vertexBytes, 0, indexBytes};
    vkCmdCopyBuffer(cmd, staging.buffer, mesh.indices.buffer, 1, &indexCopy);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });

  impl_->destroyBuffer(staging);

  impl_->meshes.push_back(mesh);
  return {static_cast<uint32_t>(impl_->meshes.size() - 1)};
}

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

  FrameData &frame = impl_->frames[impl_->frameNumber % kFramesInFlight];
  check(vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");

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

  SceneData scene;
  scene.viewProjection = camera.projection(aspect) * camera.view();
  scene.sunDirection = lighting.sunDirection;
  scene.sunColor = lighting.sunColor;
  scene.skyColor = lighting.skyColor;
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

  VkImage image = impl_->swapchainImages[imageIndex];

  transitionImage(cmd, {image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

  // The depth image is shared by both frames in flight, so wait for the
  // previous frame's depth writes before clearing it.
  constexpr VkPipelineStageFlags2 kDepthStages =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  transitionImage(cmd, {impl_->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, kDepthStages,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        kDepthStages,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT});

  VkRenderingAttachmentInfo colorAttachment{};
  colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  colorAttachment.imageView = impl_->swapchainImageViews[imageIndex];
  colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.clearValue.color = {
      {lighting.skyColor.x, lighting.skyColor.y, lighting.skyColor.z, 1.0f}};

  VkRenderingAttachmentInfo depthAttachment{};
  depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depthAttachment.imageView = impl_->depthView;
  depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.clearValue.depthStencil.depth = 0.0f;

  VkRenderingInfo renderingInfo{};
  renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  renderingInfo.renderArea = {{0, 0}, extent};
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = &depthAttachment;

  vkCmdBeginRendering(cmd, &renderingInfo);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline);

  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor = {{0, 0}, extent};
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  for (const DrawItem &item : items) {
    const GpuMesh &mesh = impl_->meshes[item.mesh.index];

    PushConstants push;
    push.model = item.transform;
    push.vertexBuffer = mesh.vertices.address;
    push.sceneBuffer = frame.sceneBuffer.address;
    vkCmdPushConstants(cmd, impl_->pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
  }

  vkCmdEndRendering(cmd);

  transitionImage(cmd, {image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE, 0});

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
    impl_->destroyBuffer(mesh.vertices);
    impl_->destroyBuffer(mesh.indices);
  }

  vkDestroyPipeline(dev, impl_->pipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->pipelineLayout, nullptr);

  vkDestroyFence(dev, impl_->uploadFence, nullptr);
  vkDestroyCommandPool(dev, impl_->uploadPool, nullptr);
  for (FrameData &frame : impl_->frames) {
    impl_->destroyBuffer(frame.sceneBuffer);
    vkDestroySemaphore(dev, frame.imageAvailable, nullptr);
    vkDestroyFence(dev, frame.renderFence, nullptr);
    vkDestroyCommandPool(dev, frame.commandPool, nullptr);
  }

  impl_->destroySwapchain();
  vmaDestroyAllocator(impl_->allocator);

  vkb::destroy_device(impl_->device);
  vkb::destroy_surface(impl_->instance, impl_->surface);
  vkb::destroy_instance(impl_->instance);
}

} // namespace meridian
