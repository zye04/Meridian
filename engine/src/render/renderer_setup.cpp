#include "renderer_impl.h"
#include "taa_upscaler.h"

#include <meridian/window.h>

#include <bit>
#include <iostream>
#include <stdexcept>

namespace meridian {

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
          // Transfer source: frames can be copied out for captures.
          .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
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

  const Extent display{swapchain.extent.width, swapchain.extent.height};
  const Extent render = scaledExtent(display, aaSettings.renderScale);
  renderExtent = {render.width, render.height};
  depth = createRenderImage(kDepthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
  createDepthPyramid();
  hdr = createRenderImage(kHdrFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
  motion = createRenderImage(kMotionFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
  transparencyMask = createRenderImage(kTransparencyMaskFormat,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
  output = createDisplayImage(kHdrFormat,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
  submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {output.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
  });
  upscaler = std::make_unique<TaaUpscaler>(*this, display);
  historyValid = false;

  // Safe because the device is idle whenever the swapchain is (re)created.
  const VkDescriptorImageInfo resolved{nearestSampler, output.view,
                                       VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet resolvedWrite{};
  resolvedWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  resolvedWrite.dstSet = postSet;
  resolvedWrite.dstBinding = 0;
  resolvedWrite.descriptorCount = 1;
  resolvedWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  resolvedWrite.pImageInfo = &resolved;
  vkUpdateDescriptorSets(device.device, 1, &resolvedWrite, 0, nullptr);

  swapchainOutOfDate = false;

  std::cout << "Swapchain: " << swapchainImages.size() << " images, "
            << swapchain.extent.width << "x" << swapchain.extent.height
            << "\n";
  std::cout << "Render " << render.width << "x" << render.height << ", display "
            << display.width << "x" << display.height << ", jitter phases "
            << jitterPhaseCount(render, display, aaSettings.rayReconstruction)
            << ", mip bias " << textureMipBias(render, display) << "\n";
}

void Renderer::Impl::createDepthPyramid() {
  pyramidValid = false;
  depthPyramidExtent = {std::bit_floor(renderExtent.width),
                        std::bit_floor(renderExtent.height)};
  const auto levels = static_cast<uint32_t>(std::bit_width(
      std::max(depthPyramidExtent.width, depthPyramidExtent.height)));
  if (levels > kMaxPyramidLevels) {
    throw std::runtime_error("screen too large for the depth pyramid");
  }
  depthPyramid = createImage(VK_FORMAT_R32_SFLOAT,
                             {depthPyramidExtent.width, depthPyramidExtent.height, 1},
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, levels);
  depthPyramidLevels.resize(levels);
  for (uint32_t level = 0; level < levels; ++level) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthPyramid.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    check(vkCreateImageView(device.device, &viewInfo, nullptr,
                            &depthPyramidLevels[level]),
          "vkCreateImageView");
  }
  submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {depthPyramid.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
  });

  for (uint32_t level = 0; level < levels; ++level) {
    const VkDescriptorImageInfo source =
        level == 0 ? VkDescriptorImageInfo{nearestSampler, depth.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                   : VkDescriptorImageInfo{minSampler, depthPyramidLevels[level - 1],
                                           VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo destination{VK_NULL_HANDLE, depthPyramidLevels[level],
                                            VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = pyramidSets[level];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[b].pImageInfo = b == 0 ? &source : &destination;
    }
    vkUpdateDescriptorSets(device.device, 2, writes, 0, nullptr);
  }
  const VkDescriptorImageInfo pyramidInfo{minSampler, depthPyramid.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet cullWrite{};
  cullWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  cullWrite.dstSet = cullSet;
  cullWrite.descriptorCount = 1;
  cullWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  cullWrite.pImageInfo = &pyramidInfo;
  vkUpdateDescriptorSets(device.device, 1, &cullWrite, 0, nullptr);
}

void Renderer::Impl::destroySwapchain() {
  upscaler.reset();
  destroyImage(output);
  destroyImage(transparencyMask);
  destroyImage(motion);
  destroyImage(hdr);
  for (VkImageView view : depthPyramidLevels) {
    vkDestroyImageView(device.device, view, nullptr);
  }
  depthPyramidLevels.clear();
  destroyImage(depthPyramid);
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
    frame.instanceStaging = createBuffer(
        kMaxInstances * sizeof(InstanceData), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.drawCountReadback = createBuffer(
        sizeof(DrawCounters), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    frame.instanceBuffer = createBuffer(
        kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
  createTlas(kMaxInstances);

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
  queryInfo.queryCount = kFramesInFlight * (kMaxPassTimes + 1);
  check(vkCreateQueryPool(dev, &queryInfo, nullptr, &timestampPool),
        "vkCreateQueryPool");
}

// Sized for `capacity` instances once, so rebuilds never allocate.
void Renderer::Impl::createTlas(uint32_t capacity) {
  destroyAccelerationStructure(tlas);
  destroyBuffer(tlasScratch);
  destroyBuffer(tlasRecords);
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
      &capacity, &tlasSizes);

  tlas = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                     tlasSizes.accelerationStructureSize);
  tlasScratch = createBuffer(
      tlasSizes.buildScratchSize,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0, scratchAlignment);
  tlasRecords = createBuffer(
      VkDeviceSize{capacity} * sizeof(VkAccelerationStructureInstanceKHR),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);
}

void Renderer::Impl::createVisibility() {
  destroyBuffer(visibility);
  const uint32_t cellCount = placementGrid.columns * placementGrid.rows;
  visibility = createBuffer(
      (VkDeviceSize{kPlacedBase} + placedCount + cellCount) * sizeof(uint32_t),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);
  // New instances start invisible; the late culling phase picks them up.
  submitNow([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, visibility.buffer, 0, VK_WHOLE_SIZE, 0);
  });
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
  VkSamplerReductionModeCreateInfo reduction{};
  reduction.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
  reduction.reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN;
  samplerInfo.pNext = &reduction;
  // Culling picks a pyramid level per instance with textureLod.
  samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &minSampler),
        "vkCreateSampler");
  samplerInfo.pNext = nullptr;

  VkDescriptorSetLayoutBinding pyramidBindings[2]{};
  for (uint32_t b = 0; b < 2; ++b) {
    pyramidBindings[b].binding = b;
    pyramidBindings[b].descriptorType = b == 0
                                            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pyramidBindings[b].descriptorCount = 1;
    pyramidBindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo pyramidLayoutInfo{};
  pyramidLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  pyramidLayoutInfo.bindingCount = 2;
  pyramidLayoutInfo.pBindings = pyramidBindings;
  check(vkCreateDescriptorSetLayout(dev, &pyramidLayoutInfo, nullptr,
                                    &pyramidSetLayout),
        "vkCreateDescriptorSetLayout");
  VkDescriptorSetLayoutBinding cullBinding = pyramidBindings[0];
  pyramidLayoutInfo.bindingCount = 1;
  pyramidLayoutInfo.pBindings = &cullBinding;
  check(vkCreateDescriptorSetLayout(dev, &pyramidLayoutInfo, nullptr, &cullSetLayout),
        "vkCreateDescriptorSetLayout");

  // One set per pyramid level, plus the culling pass's.
  VkDescriptorPoolSize pyramidPoolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxPyramidLevels + 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxPyramidLevels},
  };
  VkDescriptorPoolCreateInfo pyramidPoolInfo{};
  pyramidPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pyramidPoolInfo.maxSets = kMaxPyramidLevels + 1;
  pyramidPoolInfo.poolSizeCount = 2;
  pyramidPoolInfo.pPoolSizes = pyramidPoolSizes;
  check(vkCreateDescriptorPool(dev, &pyramidPoolInfo, nullptr, &pyramidPool),
        "vkCreateDescriptorPool");
  std::array<VkDescriptorSetLayout, kMaxPyramidLevels> pyramidSetLayouts;
  pyramidSetLayouts.fill(pyramidSetLayout);
  VkDescriptorSetAllocateInfo pyramidAlloc{};
  pyramidAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  pyramidAlloc.descriptorPool = pyramidPool;
  pyramidAlloc.descriptorSetCount = kMaxPyramidLevels;
  pyramidAlloc.pSetLayouts = pyramidSetLayouts.data();
  check(vkAllocateDescriptorSets(dev, &pyramidAlloc, pyramidSets.data()),
        "vkAllocateDescriptorSets");
  pyramidAlloc.descriptorSetCount = 1;
  pyramidAlloc.pSetLayouts = &cullSetLayout;
  check(vkAllocateDescriptorSets(dev, &pyramidAlloc, &cullSet),
        "vkAllocateDescriptorSets");

  // Must match post.glsl.
  constexpr VkShaderStageFlags kPostStages =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutBinding bindings[3]{};
  for (uint32_t b = 0; b < 3; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorType = b == 2
                                     ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[b].descriptorCount = 1;
    bindings[b].stageFlags = kPostStages;
  }

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 3;
  layoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &postSetLayout),
        "vkCreateDescriptorSetLayout");

  // Must match atmosphere.glsl and the *_lut.comp shaders: four sampled
  // LUTs, then the same four as storage images.
  VkDescriptorSetLayoutBinding atmosphereBindings[8]{};
  for (uint32_t b = 0; b < 8; ++b) {
    atmosphereBindings[b].binding = b;
    atmosphereBindings[b].descriptorType =
        b < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    atmosphereBindings[b].descriptorCount = 1;
    atmosphereBindings[b].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo atmosphereLayoutInfo{};
  atmosphereLayoutInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  atmosphereLayoutInfo.bindingCount = 8;
  atmosphereLayoutInfo.pBindings = atmosphereBindings;
  check(vkCreateDescriptorSetLayout(dev, &atmosphereLayoutInfo, nullptr,
                                    &atmosphereSetLayout),
        "vkCreateDescriptorSetLayout");

  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5},
  };
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 2;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool),
        "vkCreateDescriptorPool");

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &postSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &postSet),
        "vkAllocateDescriptorSets");

  allocInfo.pSetLayouts = &atmosphereSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &atmosphereSet),
        "vkAllocateDescriptorSets");
}

void Renderer::Impl::createAtmosphere() {
  constexpr VkFormat kLutFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
  constexpr VkImageUsageFlags kLutUsage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  // Sizes must match atmosphere.glsl. 32-bit floats: the reference notes
  // visible artifacts with 16-bit LUTs.
  transmittanceLut = createImage(kLutFormat, {256, 64, 1}, kLutUsage,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
  multiScatteringLut = createImage(kLutFormat, {32, 32, 1}, kLutUsage,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
  skyViewLut = createImage(kLutFormat, {192, 108, 1}, kLutUsage,
                           VK_IMAGE_ASPECT_COLOR_BIT);
  aerialPerspectiveLut = createImage(kLutFormat, {32, 32, 32}, kLutUsage,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
  skyIrradianceBuffer = createBuffer(
      16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);

  const Image *luts[] = {&transmittanceLut, &multiScatteringLut, &skyViewLut,
                         &aerialPerspectiveLut};
  submitNow([&](VkCommandBuffer cmd) {
    for (const Image *lut : luts) {
      transitionImage(cmd, {lut->image, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, 0,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    }
  });

  VkDescriptorImageInfo images[8]{};
  VkWriteDescriptorSet writes[8]{};
  for (uint32_t b = 0; b < 8; ++b) {
    const bool sampled = b < 4;
    images[b] = {sampled ? linearSampler : VK_NULL_HANDLE, luts[b % 4]->view,
                 VK_IMAGE_LAYOUT_GENERAL};
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = atmosphereSet;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[b].pImageInfo = &images[b];
  }
  vkUpdateDescriptorSets(device.device, 8, writes, 0, nullptr);
}

void Renderer::Impl::createGrade() {
  gradeLut = createImage(VK_FORMAT_R16G16B16A16_SFLOAT,
                         {kGradeLutSize, kGradeLutSize, kGradeLutSize},
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
  gradeBuffer = createBuffer(sizeof(GpuGrade),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             0);
  submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {gradeLut.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
  });

  VkDescriptorImageInfo sampled{linearSampler, gradeLut.view,
                                VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo storage{VK_NULL_HANDLE, gradeLut.view,
                                VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    const bool isSampled = i == 0;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = postSet;
    writes[i].dstBinding = isSampled ? 1 : 2;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = isSampled
                                   ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = isSampled ? &sampled : &storage;
  }
  vkUpdateDescriptorSets(device.device, 2, writes, 0, nullptr);
}

void Renderer::Impl::createExposureBuffers() {
  constexpr VkBufferUsageFlags kUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  histogramBuffer =
      createBuffer(kHistogramBins * sizeof(uint32_t), kUsage, 0);
  exposureBuffer = createBuffer(sizeof(ExposureState), kUsage, 0);

  ExposureState initial;
  initial.ev100 = kInitialEv100;
  initial.exposure = 1.0f / (1.2f * std::exp2(kInitialEv100));
  submitNow([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, histogramBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdUpdateBuffer(cmd, exposureBuffer.buffer, 0, sizeof(initial),
                      &initial);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });
}

void Renderer::Impl::createMaterialSystem() {
  VkDevice dev = device.device;

  materialBuffer = createBuffer(
      kMaxMaterials * sizeof(GpuMaterial),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);

  // Trilinear with 16x anisotropy: textures stay sharp at grazing angles.
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.anisotropyEnable = VK_TRUE;
  samplerInfo.maxAnisotropy = 16.0f;
  samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &textureSampler),
        "vkCreateSampler");
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  check(vkCreateSampler(dev, &samplerInfo, nullptr, &terrainSampler),
        "vkCreateSampler");

  // Must match material.glsl and dfg_lut.comp.
  VkDescriptorSetLayoutBinding bindings[3]{};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  // The terrain mesh shader and the terrain normal map's generator read the
  // heightfield from it too.
  bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures,
                 VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_MESH_BIT_EXT |
                     VK_SHADER_STAGE_COMPUTE_BIT,
                 nullptr};
  bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  // Textures can be added while frames using the set are in flight, and
  // unused slots never need to be valid.
  VkDescriptorBindingFlags bindingFlags[3] = {
      0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      0};
  VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
  flagsInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  flagsInfo.bindingCount = 3;
  flagsInfo.pBindingFlags = bindingFlags;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.pNext = &flagsInfo;
  layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layoutInfo.bindingCount = 3;
  layoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr,
                                    &materialSetLayout),
        "vkCreateDescriptorSetLayout");

  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
  };
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &materialPool),
        "vkCreateDescriptorPool");

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = materialPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &materialSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &materialSet),
        "vkAllocateDescriptorSets");

  dfgLut = createImage(VK_FORMAT_R16G16_SFLOAT, {kDfgLutSize, kDfgLutSize, 1},
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
  VkDescriptorImageInfo dfgSampled{linearSampler, dfgLut.view,
                                   VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo dfgStorage{VK_NULL_HANDLE, dfgLut.view,
                                   VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = materialSet;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &dfgSampled;
  writes[1] = writes[0];
  writes[1].dstBinding = 2;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[1].pImageInfo = &dfgStorage;
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
}

// The DFG LUT depends on nothing but the BRDF, so it's computed once.
void Renderer::Impl::computeDfgLut() {
  VkPipeline pipeline =
      createComputePipeline(device.device, "dfg_lut.comp", sceneLayout);
  submitNow([&](VkCommandBuffer cmd) {
    transitionImage(cmd, {dfgLut.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sceneLayout,
                            1, 1, &materialSet, 0, nullptr);
    vkCmdDispatch(cmd, kDfgLutSize / 8, kDfgLutSize / 8, 1);
    transitionImage(cmd, {dfgLut.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
  });
  vkDestroyPipeline(device.device, pipeline, nullptr);
}

void Renderer::Impl::createScene() {
  vertexPool = createBuffer(kMinPoolBytes, kVertexPoolUsage, 0);
  indexPool = createBuffer(kMinPoolBytes, kIndexPoolUsage, 0);
  clusterPool = createBuffer(kMinPoolBytes, kPoolUsage, 0);
  clusterVertexPool = createBuffer(kMinPoolBytes, kPoolUsage, 0);
  clusterTrianglePool = createBuffer(kMinPoolBytes, kPoolUsage, 0);
  clusterNodePool = createBuffer(kMinPoolBytes, kPoolUsage, 0);
  clusterLevelPool = createBuffer(kMinPoolBytes, kPoolUsage, 0);

  constexpr VkBufferUsageFlags kTableUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  meshTable = createBuffer(kMaxMeshes * sizeof(MeshInfo), kTableUsage, 0);
  instanceTable =
      createBuffer(kMaxInstances * sizeof(InstanceData), kTableUsage, 0);

  // Two phases of work items and draw lists (see cullInstances).
  constexpr VkBufferUsageFlags kListUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  chunks = createBuffer(VkDeviceSize{2} * kMaxChunks * 2 * sizeof(uint32_t), kListUsage, 0);
  nodeQueues = createBuffer(VkDeviceSize{3} * kMaxTraversalNodes * 2 * sizeof(uint32_t),
                           kListUsage, 0);
  workItems = createBuffer(VkDeviceSize{2} * kMaxWorkItems * 2 * sizeof(uint32_t),
                           kListUsage, 0);
  drawLists = createBuffer(VkDeviceSize{2 * kDrawBuckets} * kMaxDrawnClusters * 2 *
                               sizeof(uint32_t),
                           kListUsage, 0);
  retestClusters = createBuffer(VkDeviceSize{kMaxRetestClusters} * 2 * sizeof(uint32_t),
                                kListUsage, 0);
  drawCounters = createBuffer(
      sizeof(DrawCounters),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);
  createVisibility();
  terrainCounters = createBuffer(
      sizeof(TerrainCounters),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      0);
  // Nodes are two uints (terrain.glsl).
  constexpr VkDeviceSize kNodeSize = 2 * sizeof(uint32_t);
  terrainLists = createBuffer(VkDeviceSize{3} * kMaxTerrainNodes * kNodeSize, kListUsage, 0);
  terrain.nodes = terrainLists.address + VkDeviceSize{2} * kMaxTerrainNodes * kNodeSize;
}

void Renderer::Impl::createPipelines() {
  VkDevice dev = device.device;

  VkPushConstantRange pushRange{};
  pushRange.stageFlags = kScenePushStages;
  pushRange.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo sceneLayoutInfo{};
  sceneLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  const VkDescriptorSetLayout sceneSetLayouts[] = {atmosphereSetLayout,
                                                   materialSetLayout};
  sceneLayoutInfo.setLayoutCount = 2;
  sceneLayoutInfo.pSetLayouts = sceneSetLayouts;
  sceneLayoutInfo.pushConstantRangeCount = 1;
  sceneLayoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &sceneLayoutInfo, nullptr, &sceneLayout),
        "vkCreatePipelineLayout");

  const VkFormat sceneFormats[] = {kHdrFormat, kMotionFormat,
                                   kTransparencyMaskFormat};
  // Opaque and masked geometry is drawn as culled clusters (mesh shader),
  // blended geometry as whole meshes from the CPU (vertex shader).
  PipelineDesc meshDesc{.meshShader = "mesh.mesh",
                        .fragmentShader = "mesh.frag",
                        .layout = sceneLayout,
                        .colorFormats = sceneFormats,
                        .depthFormat = kDepthFormat,
                        .dynamicCullMode = true};
  meshPipeline = createGraphicsPipeline(dev, meshDesc);
  // Alpha testing (discard) disables early depth testing, so masked
  // materials get their own pipeline instead of slowing down opaque ones.
  meshDesc.fragmentShader = "mesh_masked.frag";
  maskedMeshPipeline = createGraphicsPipeline(dev, meshDesc);
  meshDesc.meshShader = nullptr;
  meshDesc.vertexShader = "mesh.vert";
  meshDesc.fragmentShader = "mesh_blended.frag";
  meshDesc.alphaBlend = true;
  blendedMeshPipeline = createGraphicsPipeline(dev, meshDesc);

  // The CDLOD terrain: opaque, back faces culled.
  meshDesc.vertexShader = nullptr;
  meshDesc.meshShader = "terrain.mesh";
  meshDesc.fragmentShader = "terrain.frag";
  meshDesc.alphaBlend = false;
  terrainPipeline = createGraphicsPipeline(dev, meshDesc);

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

  VkPushConstantRange gradePushRange{};
  gradePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  gradePushRange.size = sizeof(GradeConstants);
  VkPipelineLayoutCreateInfo gradeLayoutInfo = postLayoutInfo;
  gradeLayoutInfo.pPushConstantRanges = &gradePushRange;
  check(vkCreatePipelineLayout(dev, &gradeLayoutInfo, nullptr, &gradeLayout),
        "vkCreatePipelineLayout");
  gradeLutPipeline = createComputePipeline(dev, "grade_lut.comp", gradeLayout);

  VkPushConstantRange cullPushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(CullConstants)};
  VkPipelineLayoutCreateInfo cullLayoutInfo{};
  cullLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  cullLayoutInfo.setLayoutCount = 1;
  cullLayoutInfo.pSetLayouts = &cullSetLayout;
  cullLayoutInfo.pushConstantRangeCount = 1;
  cullLayoutInfo.pPushConstantRanges = &cullPushRange;
  check(vkCreatePipelineLayout(dev, &cullLayoutInfo, nullptr, &cullLayout),
        "vkCreatePipelineLayout");
  cellCullPipeline = createComputePipeline(dev, "cell_cull.comp", cullLayout);
  cullPipeline = createComputePipeline(dev, "cull.comp", cullLayout);
  clusterTraversePipeline = createComputePipeline(dev, "cluster_traverse.comp", cullLayout);
  clusterCullPipeline = createComputePipeline(dev, "cluster_cull.comp", cullLayout);

  VkPushConstantRange terrainPushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(TerrainSelectConstants)};
  VkPipelineLayoutCreateInfo terrainLayoutInfo{};
  terrainLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  terrainLayoutInfo.pushConstantRangeCount = 1;
  terrainLayoutInfo.pPushConstantRanges = &terrainPushRange;
  check(vkCreatePipelineLayout(dev, &terrainLayoutInfo, nullptr, &terrainSelectLayout),
        "vkCreatePipelineLayout");
  terrainSelectPipeline =
      createComputePipeline(dev, "terrain_select.comp", terrainSelectLayout);

  VkPushConstantRange pyramidPushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(PyramidConstants)};
  VkPipelineLayoutCreateInfo pyramidLayoutInfo{};
  pyramidLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pyramidLayoutInfo.setLayoutCount = 1;
  pyramidLayoutInfo.pSetLayouts = &pyramidSetLayout;
  pyramidLayoutInfo.pushConstantRangeCount = 1;
  pyramidLayoutInfo.pPushConstantRanges = &pyramidPushRange;
  check(vkCreatePipelineLayout(dev, &pyramidLayoutInfo, nullptr, &pyramidLayout),
        "vkCreatePipelineLayout");
  pyramidPipeline = createComputePipeline(dev, "depth_pyramid.comp", pyramidLayout);
  histogramPipeline =
      createComputePipeline(dev, "histogram.comp", postLayout);
  exposurePipeline = createComputePipeline(dev, "exposure.comp", postLayout);

  transmittanceLutPipeline =
      createComputePipeline(dev, "transmittance_lut.comp", sceneLayout);
  multiScatteringLutPipeline =
      createComputePipeline(dev, "multiscattering_lut.comp", sceneLayout);
  skyViewLutPipeline =
      createComputePipeline(dev, "sky_view_lut.comp", sceneLayout);
  aerialPerspectiveLutPipeline =
      createComputePipeline(dev, "aerial_perspective_lut.comp", sceneLayout);
  skyIrradiancePipeline =
      createComputePipeline(dev, "sky_irradiance.comp", sceneLayout);

  skyPipeline = createGraphicsPipeline(dev, {.vertexShader = "sky.vert",
                                             .fragmentShader = "sky.frag",
                                             .layout = sceneLayout,
                                             .colorFormats = sceneFormats,
                                             .depthFormat = kDepthFormat,
                                             .depthTest = false});

  const VkFormat swapchainFormat[] = {swapchain.image_format};
  tonemapPipeline =
      createGraphicsPipeline(dev, {.vertexShader = "fullscreen.vert",
                                   .fragmentShader = "tonemap.frag",
                                   .layout = postLayout,
                                   .colorFormats = swapchainFormat});
}

Renderer::Renderer(Window &window, UpscalerProvider *provider)
    : impl_(std::make_unique<Impl>()) {
  impl_->window = window.handle();
  impl_->upscalerProvider = provider;

  vkb::InstanceBuilder instanceBuilder;
  instanceBuilder.set_app_name("Meridian").require_api_version(1, 3, 0);
  if (provider) {
    instanceBuilder.enable_extensions(provider->instanceExtensions());
  }
  if (kEnableValidation) {
    instanceBuilder.request_validation_layers()
        .set_debug_callback(debugCallback)
        .set_debug_messenger_severity(
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
  }
  auto instance = instanceBuilder.build();
  if (!instance) {
    throw std::runtime_error(instance.error().message());
  }
  impl_->instance = instance.value();

  check(glfwCreateWindowSurface(impl_->instance.instance, impl_->window,
                                nullptr, &impl_->surface),
        "glfwCreateWindowSurface");

  VkPhysicalDeviceFeatures features{};
  features.samplerAnisotropy = VK_TRUE;
  // Blended surfaces blend color and the transparency mask differently
  // from the other attachments, which they leave untouched.
  features.independentBlend = VK_TRUE;
  // The terrain normal map (RG16_SNORM) is written from a compute shader.
  features.shaderStorageImageExtendedFormats = VK_TRUE;

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  features12.bufferDeviceAddress = VK_TRUE;
  // Min-reduction sampling builds the occlusion depth pyramid.
  features12.samplerFilterMinmax = VK_TRUE;
  // Bindless textures: one large, partially filled array indexed per pixel.
  features12.descriptorIndexing = VK_TRUE;
  features12.runtimeDescriptorArray = VK_TRUE;
  features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  features12.descriptorBindingPartiallyBound = VK_TRUE;
  features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
  features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;
  // discard (alpha masking) compiles to demote-to-helper in SPIR-V 1.6.
  features13.shaderDemoteToHelperInvocation = VK_TRUE;
  // glslc declares mesh shader workgroup sizes with LocalSizeId.
  features13.maintenance4 = VK_TRUE;

  // Hardware ray tracing is required: shadows (and later reflections and GI)
  // are traced with ray queries.
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{};
  accelerationFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
  accelerationFeatures.accelerationStructure = VK_TRUE;

  VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
  meshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
  meshShaderFeatures.meshShader = VK_TRUE;

  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
  rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
  rayQueryFeatures.rayQuery = VK_TRUE;

  auto physical =
      vkb::PhysicalDeviceSelector{impl_->instance}
          .set_minimum_version(1, 3)
          .set_required_features(features)
          .set_required_features_12(features12)
          .set_required_features_13(features13)
          .add_required_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
          .add_required_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME)
          .add_required_extension(
              VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
          .add_required_extension_features(accelerationFeatures)
          .add_required_extension_features(rayQueryFeatures)
          .add_required_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME)
          .add_required_extension_features(meshShaderFeatures)
          .set_surface(impl_->surface)
          .select();
  if (!physical) {
    throw std::runtime_error(physical.error().message());
  }
  if (provider) {
    for (const char *extension : provider->deviceExtensions(
             impl_->instance.instance, physical.value().physical_device)) {
      physical.value().enable_extension_if_present(extension);
    }
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
  if (provider) {
    provider->init({impl_->instance.instance, physical.value().physical_device,
                    impl_->device.device, impl_->instance.fp_vkGetInstanceProcAddr,
                    impl_->device.fp_vkGetDeviceProcAddr});
  }

  VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{};
  accelerationProperties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties{};
  meshProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
  accelerationProperties.pNext = &meshProperties;
  VkPhysicalDeviceVulkan13Properties properties13{};
  properties13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
  meshProperties.pNext = &properties13;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &accelerationProperties;
  vkGetPhysicalDeviceProperties2(physical.value().physical_device, &properties);
  if (meshProperties.maxMeshWorkGroupCount[0] < kMaxDrawnClusters ||
      meshProperties.maxMeshWorkGroupTotalCount < kMaxDrawnClusters ||
      properties.properties.limits.maxComputeWorkGroupCount[0] < kMaxWorkItems) {
    throw std::runtime_error("GPU dispatch limits are below the culling list sizes");
  }
  impl_->scratchAlignment =
      accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
  impl_->maxBufferSize = properties13.maxBufferSize;

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
  impl_->createMaterialSystem();
  impl_->createPipelines();
  impl_->computeDfgLut();
  impl_->createExposureBuffers();
  impl_->createGrade();
  impl_->createAtmosphere();
  impl_->createScene();

  std::cout << "GPU: " << physical.value().name << "\n";
}

Renderer::~Renderer() {
  VkDevice dev = impl_->device.device;
  vkDeviceWaitIdle(dev);

  for (GpuMesh &mesh : impl_->meshes) {
    impl_->destroyAccelerationStructure(mesh.blas);
  }
  for (AccelerationStructure &blas : impl_->prototypeBlas) {
    impl_->destroyAccelerationStructure(blas);
  }
  for (Buffer *buffer :
       {&impl_->vertexPool, &impl_->indexPool, &impl_->meshTable,
        &impl_->instanceTable, &impl_->workItems, &impl_->drawLists, &impl_->drawCounters,
        &impl_->retestClusters,
        &impl_->clusterPool, &impl_->clusterVertexPool, &impl_->clusterTrianglePool,
        &impl_->clusterNodePool, &impl_->clusterLevelPool, &impl_->nodeQueues,
        &impl_->visibility, &impl_->terrainCounters, &impl_->terrainLists,
        &impl_->terrainPyramid, &impl_->placedInstances, &impl_->cells, &impl_->chunks,
        &impl_->prototypeTable, &impl_->prototypeMeshTable, &impl_->tlasRecords}) {
    impl_->destroyBuffer(*buffer);
  }

  vkDestroyPipeline(dev, impl_->tonemapPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->gradeLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->skyPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->skyIrradiancePipeline, nullptr);
  vkDestroyPipeline(dev, impl_->aerialPerspectiveLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->skyViewLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->multiScatteringLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->transmittanceLutPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->exposurePipeline, nullptr);
  vkDestroyPipeline(dev, impl_->histogramPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->pyramidPipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->pyramidLayout, nullptr);
  vkDestroyPipeline(dev, impl_->cellCullPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->cullPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->clusterTraversePipeline, nullptr);
  vkDestroyPipeline(dev, impl_->clusterCullPipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->cullLayout, nullptr);
  vkDestroyPipelineLayout(dev, impl_->postLayout, nullptr);
  vkDestroyPipelineLayout(dev, impl_->gradeLayout, nullptr);
  vkDestroyPipeline(dev, impl_->meshPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->maskedMeshPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->blendedMeshPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->terrainPipeline, nullptr);
  vkDestroyPipeline(dev, impl_->terrainSelectPipeline, nullptr);
  vkDestroyPipelineLayout(dev, impl_->terrainSelectLayout, nullptr);
  vkDestroyPipelineLayout(dev, impl_->sceneLayout, nullptr);

  vkDestroyQueryPool(dev, impl_->timestampPool, nullptr);
  vkDestroyFence(dev, impl_->uploadFence, nullptr);
  vkDestroyCommandPool(dev, impl_->uploadPool, nullptr);
  impl_->destroyAccelerationStructure(impl_->tlas);
  impl_->destroyBuffer(impl_->tlasScratch);
  for (FrameData &frame : impl_->frames) {
    impl_->destroyBuffer(frame.instanceBuffer);
    impl_->destroyBuffer(frame.instanceStaging);
    impl_->destroyBuffer(frame.sceneBuffer);
    impl_->destroyBuffer(frame.drawCountReadback);
    vkDestroySemaphore(dev, frame.imageAvailable, nullptr);
    vkDestroyFence(dev, frame.renderFence, nullptr);
    vkDestroyCommandPool(dev, frame.commandPool, nullptr);
  }

  for (Image &texture : impl_->textures) {
    impl_->destroyImage(texture);
  }
  impl_->destroyImage(impl_->dfgLut);
  impl_->destroyImage(impl_->terrainHeightImage);
  impl_->destroyImage(impl_->terrainNormalImage);
  vkDestroySampler(dev, impl_->terrainSampler, nullptr);
  impl_->destroyBuffer(impl_->materialBuffer);
  vkDestroyDescriptorPool(dev, impl_->materialPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->materialSetLayout, nullptr);
  vkDestroySampler(dev, impl_->textureSampler, nullptr);
  impl_->destroyBuffer(impl_->skyIrradianceBuffer);
  impl_->destroyImage(impl_->aerialPerspectiveLut);
  impl_->destroyImage(impl_->skyViewLut);
  impl_->destroyImage(impl_->multiScatteringLut);
  impl_->destroyImage(impl_->transmittanceLut);
  impl_->destroyImage(impl_->gradeLut);
  impl_->destroyBuffer(impl_->gradeBuffer);
  impl_->destroyBuffer(impl_->exposureBuffer);
  impl_->destroyBuffer(impl_->histogramBuffer);
  impl_->destroySwapchain();
  vkDestroyDescriptorPool(dev, impl_->pyramidPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->pyramidSetLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->cullSetLayout, nullptr);
  vkDestroySampler(dev, impl_->minSampler, nullptr);
  vkDestroyDescriptorPool(dev, impl_->descriptorPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->atmosphereSetLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, impl_->postSetLayout, nullptr);
  vkDestroySampler(dev, impl_->linearSampler, nullptr);
  vkDestroySampler(dev, impl_->nearestSampler, nullptr);
  vmaDestroyAllocator(impl_->allocator);

  vkb::destroy_device(impl_->device);
  vkb::destroy_surface(impl_->instance, impl_->surface);
  vkb::destroy_instance(impl_->instance);
}

} // namespace meridian
