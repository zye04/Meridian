#include "renderer_impl.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace meridian {

void Renderer::Impl::createReference() {
  VkDevice dev = device.device;
  const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                             VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
  setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setLayoutInfo.bindingCount = 1;
  setLayoutInfo.pBindings = &binding;
  check(vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &referenceSetLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &referencePool), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = referencePool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &referenceSetLayout;
  check(vkAllocateDescriptorSets(dev, &allocInfo, &referenceSet), "vkAllocateDescriptorSets");

  // Sets 0 and 1 as in the scene layout (atmosphere LUTs, bindless).
  const VkDescriptorSetLayout layouts[] = {atmosphereSetLayout, materialSetLayout,
                                           referenceSetLayout};
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReferenceConstants)};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 3;
  layoutInfo.pSetLayouts = layouts;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &referenceLayout),
        "vkCreatePipelineLayout");
  referencePipeline = createComputePipeline(dev, "reference.comp", referenceLayout);
}

void Renderer::Impl::destroyReference() {
  VkDevice dev = device.device;
  destroyBuffer(referenceSum);
  destroyBuffer(referenceSquares);
  vkDestroyPipeline(dev, referencePipeline, nullptr);
  vkDestroyPipelineLayout(dev, referenceLayout, nullptr);
  vkDestroyDescriptorPool(dev, referencePool, nullptr);
  vkDestroyDescriptorSetLayout(dev, referenceSetLayout, nullptr);
}

void Renderer::beginReference(ReferenceMode mode) {
  if (!impl_->rayTracing) {
    throw std::runtime_error("beginReference needs setRayTracing(true)");
  }
  impl_->referenceMode = mode;
  impl_->resetReference();
  impl_->referencing = true;
}

// Sizes the accumulation to the display, clears it and points the output
// binding at the current output image; again after a swapchain recreation,
// which replaces both.
void Renderer::Impl::resetReference() {
  Impl &impl = *this;
  check(vkDeviceWaitIdle(impl.device.device), "vkDeviceWaitIdle");
  const VkExtent2D extent = impl.swapchain.extent;
  const VkDeviceSize bytes = VkDeviceSize{extent.width} * extent.height * 16;
  if (impl.referenceSum.size != bytes) {
    impl.destroyBuffer(impl.referenceSum);
    impl.destroyBuffer(impl.referenceSquares);
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    impl.referenceSum = impl.createBuffer(bytes, usage, 0);
    impl.referenceSquares = impl.createBuffer(bytes, usage, 0);
  }
  impl.submitNow([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, impl.referenceSum.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, impl.referenceSquares.buffer, 0, VK_WHOLE_SIZE, 0);
  });
  // The output image is recreated with the swapchain; rewrite every time.
  const VkDescriptorImageInfo info{VK_NULL_HANDLE, impl.output.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = impl.referenceSet;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(impl.device.device, 1, &write, 0, nullptr);
  impl.referenceCount = 0;
}

uint32_t Renderer::referenceSamples() const { return impl_->referenceCount; }

void Renderer::Impl::dispatchReference(VkCommandBuffer cmd, const Camera &camera,
                                       VkDeviceAddress scene) {
  const VkExtent2D extent = swapchain.extent;
  ReferenceConstants constants;
  constants.sceneBuffer = scene;
  constants.sum = referenceSum.address;
  constants.squares = referenceSquares.address;
  constants.frame = referenceCount;
  constants.mode = static_cast<uint32_t>(referenceMode);
  const Vec3 forward = camera.forward();
  const Vec3 right = camera.right();
  const Vec3 up = cross(right, forward);
  for (int c = 0; c < 3; ++c) {
    constants.cameraRight[c] = (&right.x)[c];
    constants.cameraUp[c] = (&up.x)[c];
    constants.cameraForward[c] = (&forward.x)[c];
  }
  constants.aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
  constants.tanHalfFovY = std::tan(0.5f * camera.fovY);
  constants.width = extent.width;
  constants.height = extent.height;
  // Throwaway (pt-targets, not for merge): light split bits, see reference.comp.
  if (const char *split = std::getenv("MERIDIAN_REFERENCE_SPLIT")) {
    constants.pad = static_cast<uint32_t>(std::atoi(split));
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, referencePipeline);
  const VkDescriptorSet sets[] = {atmosphereSet, materialSet, referenceSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, referenceLayout, 0, 3, sets, 0,
                          nullptr);
  vkCmdPushConstants(cmd, referenceLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                     &constants);
  vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
  ++referenceCount;
}

double Renderer::referenceNoise() {
  Impl &impl = *impl_;
  if (impl.referenceCount < 2) {
    return 1.0;
  }
  check(vkDeviceWaitIdle(impl.device.device), "vkDeviceWaitIdle");
  const VkDeviceSize bytes = impl.referenceSum.size;
  Buffer staging = impl.createBuffer(
      2 * bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  impl.submitNow([&](VkCommandBuffer cmd) {
    const VkBufferCopy sum{0, 0, bytes};
    const VkBufferCopy squares{0, bytes, bytes};
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyBuffer(cmd, impl.referenceSum.buffer, staging.buffer, 1, &sum);
    vkCmdCopyBuffer(cmd, impl.referenceSquares.buffer, staging.buffer, 1, &squares);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(impl.allocator, staging.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  const auto *sums = static_cast<const float *>(staging.mapped);
  const float *squareSums = sums + bytes / sizeof(float);
  const uint32_t width = impl.swapchain.extent.width;
  const uint32_t height = impl.swapchain.extent.height;
  constexpr uint32_t kTile = 16;
  std::vector<double> errors;
  for (uint32_t ty = 0; ty + kTile <= height; ty += kTile) {
    for (uint32_t tx = 0; tx + kTile <= width; tx += kTile) {
      double meanSum = 0.0, varianceSum = 0.0, n = 0.0;
      for (uint32_t y = ty; y < ty + kTile; ++y) {
        for (uint32_t x = tx; x < tx + kTile; ++x) {
          const float *p = sums + 4 * (size_t{y} * width + x);
          n = p[3];
          const double mean = (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / n;
          const double meanOfSquares = squareSums[4 * (size_t{y} * width + x)] / n;
          meanSum += mean;
          varianceSum += std::max(meanOfSquares - mean * mean, 0.0);
        }
      }
      const double pixels = kTile * kTile;
      const double tileMean = meanSum / pixels;
      if (tileMean > 1e-6 && n > 1.0) {
        errors.push_back(std::sqrt(varianceSum / (n - 1.0)) / pixels / tileMean);
      }
    }
  }
  impl.destroyBuffer(staging);
  if (errors.empty()) {
    return 0.0;
  }
  std::nth_element(errors.begin(), errors.begin() + errors.size() / 2, errors.end());
  return errors[errors.size() / 2];
}

} // namespace meridian
