#include "renderer_impl.h"

#include <cstring>

namespace meridian {

// The visibility resolve (visibility_resolve.frag): its descriptor set (the
// id target, the stats counters), layout and pipeline. Created with the
// render targets while the visibility buffer is on.
void Renderer::Impl::createVisibilityResolve() {
  VkDevice dev = device.device;
  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1] = bindings[0];
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 2;
  layoutInfo.pBindings = bindings;
  check(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &visibilitySetLayout),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &visibilityPool),
        "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = visibilityPool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &visibilitySetLayout;
  check(vkAllocateDescriptorSets(dev, &alloc, &visibilitySet), "vkAllocateDescriptorSets");

  resolveStats = createBuffer(2 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memset(resolveStats.mapped, 0, 2 * sizeof(uint32_t));
  check(vmaFlushAllocation(allocator, resolveStats.allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");

  const VkDescriptorImageInfo idInfo{nearestSampler, visibilityId.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkDescriptorBufferInfo statsInfo{resolveStats.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[2]{};
  for (uint32_t b = 0; b < 2; ++b) {
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = visibilitySet;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = bindings[b].descriptorType;
  }
  writes[0].pImageInfo = &idInfo;
  writes[1].pBufferInfo = &statsInfo;
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

  VkPushConstantRange pushRange{kScenePushStages, 0, sizeof(PushConstants)};
  const VkDescriptorSetLayout setLayouts[] = {atmosphereSetLayout, materialSetLayout,
                                              visibilitySetLayout};
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 3;
  pipelineLayoutInfo.pSetLayouts = setLayouts;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushRange;
  check(vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &resolveLayout),
        "vkCreatePipelineLayout");
  // Scene attachments 0-6; the transparency mask (2) is not written.
  const VkFormat formats[] = {kHdrFormat,    kMotionFormat,          kTransparencyMaskFormat,
                              kAlbedoFormat, kAlbedoFormat,          kNormalRoughnessFormat,
                              kSunLightFormat};
  // Attachments 0-6 except 2, which only the blended surfaces write.
  constexpr uint32_t kResolvedTargets = 0x7Bu;
  resolvePipeline = createGraphicsPipeline(dev, {.vertexShader = "fullscreen.vert",
                                                 .fragmentShader = "visibility_resolve.frag",
                                                 .layout = resolveLayout,
                                                 .colorFormats = formats,
                                                 .depthTest = false,
                                                 .colorWriteMaskBits = kResolvedTargets});
}

void Renderer::Impl::destroyVisibilityResolve() {
  VkDevice dev = device.device;
  vkDestroyPipeline(dev, resolvePipeline, nullptr);
  vkDestroyPipelineLayout(dev, resolveLayout, nullptr);
  vkDestroyDescriptorPool(dev, visibilityPool, nullptr);
  vkDestroyDescriptorSetLayout(dev, visibilitySetLayout, nullptr);
  resolvePipeline = VK_NULL_HANDLE;
  resolveLayout = VK_NULL_HANDLE;
  visibilityPool = VK_NULL_HANDLE;
  visibilitySet = VK_NULL_HANDLE;
  visibilitySetLayout = VK_NULL_HANDLE;
  if (resolveStats.buffer != VK_NULL_HANDLE) {
    destroyBuffer(resolveStats);
  }
  resolveStats = {};
}

Renderer::VisibilityResolveStats Renderer::visibilityResolveStats() const {
  if (impl_->resolveStats.buffer == VK_NULL_HANDLE) {
    return {};
  }
  check(vmaInvalidateAllocation(impl_->allocator, impl_->resolveStats.allocation, 0,
                                VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  const auto *counts = static_cast<const uint32_t *>(impl_->resolveStats.mapped);
  return {counts[0], counts[1]};
}

void Renderer::resetVisibilityResolveStats() {
  if (impl_->resolveStats.buffer == VK_NULL_HANDLE) {
    return;
  }
  // Frames in flight still add to the counters.
  check(vkDeviceWaitIdle(impl_->device.device), "vkDeviceWaitIdle");
  std::memset(impl_->resolveStats.mapped, 0, 2 * sizeof(uint32_t));
  check(vmaFlushAllocation(impl_->allocator, impl_->resolveStats.allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");
}

} // namespace meridian
